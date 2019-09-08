/* Copyright 2019 Patrick Kidger. All Rights Reserved.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * 
 *    http://www.apache.org/licenses/LICENSE-2.0
 * 
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 * ========================================================================= */


#include <torch/extension.h>
#include <cstdint>    // int64_t
#include <tuple>      // std::tie, std::tuple
#include <vector>     // std::vector

#include "misc.hpp"
#include "pycapsule.hpp"
#include "signature.hpp"
#include "tensor_algebra_ops.hpp"


// TODO: add sparse computations
// TODO: try doing the word->brackets by manually computing the inverse and then using torch.sparse.mm?
// TODO: switch to pytest over unittest; rationalise some tests when we do
// TODO: check for interrupts + release GIL?
// TODO: add handling of (... x stream x channel) format
// TODO: signature_jacobian, logsignature_jacobian
// TODO: tensorflow?
// TODO: support torchscript? https://pytorch.org/tutorials/advanced/torch_script_custom_ops.html
// TODO: concatenating onto an already existing signature. A class that takes data and spits out signatures?


namespace signatory {
    namespace detail {
        // Takes the path and basepoint and returns the path increments
        torch::Tensor compute_path_increments(torch::Tensor path, torch::Tensor basepoint_value,
                                              const misc::SigSpec& sigspec) {
            int64_t num_increments {sigspec.input_stream_size - 1};
            if (sigspec.basepoint) {
                torch::Tensor path_increments = path.clone();
                path_increments.narrow(/*dim=*/stream_dim, /*start=*/0, /*len=*/1) -= basepoint_value;
                path_increments.narrow(/*dim=*/stream_dim, /*start=*/1, /*len=*/num_increments) -=
                        path.narrow(/*dim=*/stream_dim, /*start=*/0, /*len=*/num_increments);
                return path_increments;
            }
            else {
                return path.narrow(/*dim=*/stream_dim, /*start=*/1, /*len=*/num_increments) -
                       path.narrow(/*dim=*/stream_dim, /*start=*/0, /*len=*/num_increments);
            }
        }

        // Computes the backward pass through the path increments operation.
        // Returns the gradients for the original path, and for the basepoint.
        std::tuple<torch::Tensor, torch::Tensor>
        compute_path_increments_backward(torch::Tensor grad_path_increments, const misc::SigSpec& sigspec) {
            int64_t num_increments{sigspec.input_stream_size - 1};
            if (sigspec.basepoint) {
                torch::Tensor grad_path = grad_path_increments.clone();
                grad_path.narrow(/*dim=*/stream_dim, /*start=*/0, /*len=*/num_increments)
                        -= grad_path_increments.narrow(/*dim=*/stream_dim, /*start=*/1, /*len=*/num_increments);
                return {grad_path, -grad_path_increments.narrow(/*dim=*/stream_dim,
                                                                /*start=*/0,
                                                                /*len=*/1).squeeze(stream_dim)};
            }
            else {
                torch::Tensor grad_path = torch::empty({sigspec.input_stream_size,
                                                        sigspec.batch_size,
                                                        sigspec.input_channels},
                                                       sigspec.opts);
                grad_path.narrow(/*dim=*/stream_dim, /*start=*/0, /*len=*/1).zero_();
                grad_path.narrow(/*dim=*/stream_dim, /*start=*/1, /*len=*/num_increments).copy_(grad_path_increments);
                grad_path.narrow(/*dim=*/stream_dim, /*start=*/0, /*len=*/num_increments) -= grad_path_increments;
                // no second return value in this case
                return {grad_path, torch::empty({0}, sigspec.opts)};
            }
        }
    }  // namespace signatory::detail

    std::tuple<torch::Tensor, py::object>
    signature_forward(torch::Tensor path, s_size_type depth, bool stream, bool basepoint, torch::Tensor basepoint_value)
    {
        // No sense keeping track of gradients when we have a dedicated backwards function (and in-place operations mean
        // that in any case one cannot autograd through this function)
        path = path.detach();
        basepoint_value = basepoint_value.detach();
        misc::checkargs(path, depth, basepoint, basepoint_value);

        // Convert from (batch, stream, channel) to (stream, batch, channel), which is the representation we use
        // internally.
        // having 'path' have non-monotonically-decreasing strides doesn't slow things down very much, as 'path' is only
        // really used to compute 'path_increments' below, and the extra speed from a more efficient internal
        // representation more than compensates
        path = path.transpose(0, 1);
        if (!path.is_floating_point()) {
            path = path.to(torch::kFloat32);
        }
        if (basepoint) {
            // basepoint_value has dimensions (batch, channel) so we don't need to switch anything
            basepoint_value = basepoint_value.to(path.dtype());
        }

        misc::SigSpec sigspec{path, depth, stream, basepoint};

        torch::Tensor path_increments = detail::compute_path_increments(path, basepoint_value, sigspec);

        // We allocate memory for certain things upfront.
        // We want to construct things in-place wherever possible. Signatures get large; this saves a lot of time.

        torch::Tensor signature;
        std::vector<torch::Tensor> signature_by_term;
        std::vector<torch::Tensor> signature_by_term_at_stream;
        if (stream) {
            // if stream == true then we want to store all intermediate results
            signature = torch::empty({sigspec.output_stream_size,
                                      sigspec.batch_size,
                                      sigspec.output_channels},
                                     sigspec.opts);
            torch::Tensor first_term = signature.narrow(/*dim=*/stream_dim,
                                                        /*start=*/0,
                                                        /*len=*/1).squeeze(stream_dim);
            misc::slice_by_term(signature, signature_by_term, sigspec);
            misc::slice_by_term(first_term, signature_by_term_at_stream, sigspec);
        }
        else {
            signature = torch::empty({sigspec.batch_size, sigspec.output_channels}, sigspec.opts);
            misc::slice_by_term(signature, signature_by_term_at_stream, sigspec);
        }

        // compute the first term
        ta_ops::restricted_exp(path_increments.narrow(/*dim=*/stream_dim, /*start=*/0, /*len=*/1).squeeze(stream_dim),
                               signature_by_term_at_stream, sigspec);

        for (int64_t stream_index = 1; stream_index < sigspec.output_stream_size; ++stream_index) {
            if (stream) {
                signature.narrow(/*dim=*/stream_dim,
                                 /*start=*/stream_index,
                                 /*len=*/1).copy_(signature.narrow(/*dim=*/stream_dim,
                                                                   /*start=*/stream_index - 1,
                                                                   /*len=*/1));
                misc::slice_at_stream(signature_by_term, signature_by_term_at_stream, stream_index);
            }
            ta_ops::mult_fused_restricted_exp(path_increments.narrow(/*dim=*/stream_dim,
                                                                     /*start=*/stream_index,
                                                                     /*len=*/1).squeeze(stream_dim),
                                              signature_by_term_at_stream,
                                              sigspec);
        }
        py::object backwards_info_capsule = misc::wrap_capsule<misc::BackwardsInfo>(std::move(sigspec),
                                                                                    signature_by_term,
                                                                                    signature,
                                                                                    path_increments);

        // TODO: uncomment when 24413 is fixed
        // We have to do the transpose in the Python side to avoid PyTorch bug 24413.
        // https://github.com/pytorch/pytorch/issues/24413
//        torch::Tensor out = misc::transpose(out, sigspec);

        return {signature, backwards_info_capsule};
    }

    std::tuple<torch::Tensor, torch::Tensor>
    signature_backward(torch::Tensor grad_signature, py::object backwards_info_capsule, bool clone) {
        misc::BackwardsInfo* backwards_info = misc::unwrap_capsule<misc::BackwardsInfo>(backwards_info_capsule);

        // Unpacked backwards_info
        const misc::SigSpec& sigspec = backwards_info->sigspec;
        const std::vector<torch::Tensor>& signature_by_term = backwards_info->signature_by_term;
        torch::Tensor signature = backwards_info->signature;
        torch::Tensor path_increments = backwards_info->path_increments;

        // TODO: remove when 24413 is fixed. Here we undo the transposing that autograd has done for us in the
        //  pulled-out transposes
        grad_signature = misc::transpose_reverse(grad_signature, sigspec);

        // Check arguments
        misc::checkargs_backward(grad_signature, sigspec);

        // Transpose and clone. (Clone so we don't leak changes through grad_out.)
        grad_signature = misc::transpose_reverse(grad_signature, sigspec);
        if (!grad_signature.is_floating_point()) {
            grad_signature = grad_signature.to(torch::kFloat32);
        }

        // When computing the signature we essentially did a lot of computations of the form
        // A \otimes exp(b),
        // where A is a generic member of the tensor algebra, and b is a member of the lowest nonscalar part of the
        // tensor algebra.
        // Then signature_by_term_at_stream represents A.
        // grad_signature_by_term_at_stream represents the gradient on A \otimes exp(b).
        // Note the asymmetry.
        std::vector<torch::Tensor> grad_signature_by_term_at_stream;
        std::vector<torch::Tensor> signature_by_term_at_stream;

        // There's some differences between the stream==true and stream==false cases.
        // The essential difference is that in the stream==true case, we have recorded a lot more information, which we
        // can just use. In the stream==false case this information must be recomputed.

        torch::Tensor grad_signature_at_stream;
        if (sigspec.stream) {
            grad_signature_at_stream = grad_signature.narrow(/*dim=*/stream_dim,
                                                             /*start=*/-1,
                                                             /*length=*/1).squeeze(stream_dim);
        }
        else {
            grad_signature_at_stream = grad_signature;
        }

        if (clone) {
            // Normally we want to clone so as not to leak changes.
            // This is provided as an option specifically for logsignature_backward: we control the input to
            // signature_backward in this case, so then we know that we don't need to clone.
            grad_signature_at_stream = grad_signature_at_stream.clone();
        }
        misc::slice_by_term(grad_signature_at_stream, grad_signature_by_term_at_stream, sigspec);

        if (sigspec.stream) {
            // if sigspec.stream then we already know the signature of x_1, ... x_k because we saved it as our result,
            // and we don't need to worry about recomputing it (c.f. the else branch below).
            if (sigspec.output_stream_size < 2) {
                // However if sigspec.output_stream_size is so small that we never even enter the for loop below. In
                // which case signature_by_term_at_stream isn't set. We fix that here for the sake of
                // restricted_exp_backward after the for loop, which requires it to be set.
                misc::slice_at_stream(signature_by_term, signature_by_term_at_stream, 0);
            }
        }
        else {
            // We're going to recompute the signature, as we need it to perform the gradient computations.
            // In particular we compute it backwards (which is possible via a particular reversibility property of the
            // signature), in the sense that given some input path x_1, ... x_n we compute the signature of
            // x_1, ... x_k for all k: during the forward pass we did this for k going from 2 to n. During this backward
            // pass we do it for k going from n to 2.
            // In particular we clone the signature here as we're going to modify it in-place during these computations
            // and we don't want to leak changes to the original output.
            misc::slice_by_term(signature.clone(), signature_by_term_at_stream, sigspec);
        }

        torch::Tensor grad_path_increments = torch::empty({sigspec.output_stream_size,
                                                           sigspec.batch_size,
                                                           sigspec.input_channels},
                                                          sigspec.opts);

        for (int64_t stream_index = sigspec.output_stream_size - 1; stream_index >= 1; --stream_index) {
            torch::Tensor grad_next = grad_path_increments.narrow(/*dim=*/stream_dim,
                                                                  /*start=*/stream_index,
                                                                  /*len=*/1).squeeze(stream_dim);
            torch::Tensor next = path_increments.narrow(/*dim=*/stream_dim,
                                                        /*start=*/stream_index,
                                                        /*len=*/1).squeeze(stream_dim);

            if (sigspec.stream) {
                // Just look up signature_by_term_at_stream because we saved it for output
                misc::slice_at_stream(signature_by_term, signature_by_term_at_stream, stream_index - 1);
            }
            else {
                // Recompute signature_by_term_at_stream
                ta_ops::mult_fused_restricted_exp(-next, signature_by_term_at_stream, sigspec);
            }

            ta_ops::mult_fused_restricted_exp_backward(grad_next, grad_signature_by_term_at_stream, next,
                                                       signature_by_term_at_stream, sigspec);

            if (sigspec.stream) {
                // If sigspec.stream then gradients may well have accumulated on the signatures of the partial paths, so
                // add those on here.
                grad_signature_at_stream += grad_signature.narrow(/*dim=*/stream_dim,
                                                                  /*start=*/stream_index - 1,
                                                                  /*len=*/1).squeeze(stream_dim);
            }
        }

        torch::Tensor grad_in = grad_path_increments.narrow(/*dim=*/stream_dim,
                                                            /*start=*/0,
                                                            /*len=*/1).squeeze(stream_dim);
        torch::Tensor in = path_increments.narrow(/*dim=*/stream_dim,
                                                  /*start=*/0,
                                                  /*len=*/1).squeeze(stream_dim);
        ta_ops::restricted_exp_backward(grad_in, grad_signature_by_term_at_stream, in, signature_by_term_at_stream,
                                        sigspec);


        // Find the gradient on the path from the gradient on the path increments.
        torch::Tensor grad_path;
        torch::Tensor grad_basepoint_value;
        std::tie(grad_path, grad_basepoint_value) = detail::compute_path_increments_backward(grad_path_increments,
                                                                                             sigspec);
        // convert from (stream, batch, channel) to (batch, stream, channel)
        grad_path = grad_path.transpose(0, 1);
        return {grad_path, grad_basepoint_value};
    }
}  // namespace signatory
