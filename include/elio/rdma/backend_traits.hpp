#pragma once

/// @file backend_traits.hpp
/// @brief Backend hook surface (stage S0 stub).
///
/// Two coexisting injection mechanisms, finalised in stage S2:
///
///   1. Template traits: `elio::rdma::backend_traits<B>` concept
///      requiring static `post_send / post_recv / post_rdma_write /
///      post_rdma_read` member functions. Zero-overhead static dispatch;
///      every `connection<B>` instantiation inlines through.
///
///   2. Pure-virtual `polymorphic_backend`: runtime-replaceable backend.
///      `connection<polymorphic_backend>` is a specialisation that
///      dispatches via the vtable, so users can swap backends at runtime
///      (e.g. mock vs ibverbs in the same binary).
///
/// At S0 only the namespace and forward declaration are present so other
/// headers can name the type.

#include <elio/rdma/types.hpp>

namespace elio::rdma {

/// Pure-virtual backend interface (defined in S2). Users derive from this
/// for runtime polymorphism; `connection<polymorphic_backend>` will route
/// through the vtable.
struct polymorphic_backend;

}  // namespace elio::rdma
