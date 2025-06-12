// =============================================================================
//
//  opentransport_impl.h
//  classic_mac
//
//  Created by Gemini on 2024-05-15.
//  REVISED - v2.0
//
//  This header file provides the public interface for the OpenTransport
//  implementation of the network abstraction layer.
//
//  Its sole purpose is to expose the "factory" function,
//  GetOpenTransportOperations(), which returns the table of function pointers
//  that the rest of the application will use to perform network operations.
//
//  All other functions related to the OpenTransport implementation are now
//  correctly defined as `static` within opentransport_impl.c and are not
//  exposed here. This improves encapsulation and stability.
//
// =============================================================================

#ifndef OPENTRANSPORT_IMPL_H
#define OPENTRANSPORT_IMPL_H

#include "network_abstraction.h"

/*
 * Include the core OpenTransport headers.
 * This is necessary because the NetworkOperations struct, while abstract,
 * may use types defined in these headers (e.g., EndpointRef cast to a void*).
 * Including them here ensures any file that interacts with the abstraction
 * layer has access to the necessary system types.
 */
#include <OpenTransport.h>
#include <OpenTptInternet.h>
#include <OpenTptClient.h>

/*
 * Define to indicate that the OpenTransport implementation is available
 * in the project. This can be used for conditional compilation if needed.
 */
#define HAVE_OPENTRANSPORT 1

/**
 * @brief Gets the function table for the OpenTransport network implementation.
 *
 * This is the primary entry point for the network abstraction layer to access
 * the OpenTransport-specific functions. It returns a pointer to a static
 * NetworkOperations struct containing all the necessary function pointers.
 *
 * @return A pointer to the NetworkOperations struct for OpenTransport.
 */
NetworkOperations *GetOpenTransportOperations(void);


#endif /* OPENTRANSPORT_IMPL_H */