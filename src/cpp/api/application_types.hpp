/* C++ interface to Ivy. See ../../version.h for the copyright notice. */
/**
 * @file application_types.hpp
 * @ingroup ivy_cpp_api
 * @brief Owned name and numeric network endpoint of an Ivy application.
 */
#if !defined(IVY_CPP_API_HEADERS)
#include "../ivy.hpp"
#else
#pragma once
// IVY_CPP_API_BEGIN
namespace ivy {

/** @addtogroup ivy_cpp_api
 * @{
 */
/** @brief Owned metadata snapshot, independent of the peer's later lifetime. */
struct ApplicationInfo {
    std::string name; ///< Name advertised by this application; not necessarily unique.
    std::string address; ///< Numeric IPv4/IPv6 address; IPv6 may include a scope identifier.
    std::uint16_t port = 0; ///< Advertised Ivy TCP listening port; zero before the handshake.
};
/** @} */

} // namespace ivy
// IVY_CPP_API_END
#endif
