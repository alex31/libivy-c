/* Private bridge between libivy and its C++ wrapper. Not installed. */
#pragma once

#include "ivy.h"
#include "ivysocket.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Owned, counted strings, independent of the context/peer after the copy.
 * Use a fresh/freed output for each call. Errors leave it empty. No delimiter
 * encoding is used. Free in libivy, including across DLL/CRT boundaries.
 * These symbols are linkable by libivy-cpp but are not a public/stable C API. */
typedef struct {
    char **items;
    size_t count;
} IvyStringSnapshot;

/* The two entries are application name, then host. */
int IvyContextCopyApplicationInternal(IvyContext *ctx, IvyClientPtr peer,
    IvyStringSnapshot *snapshot);
/* Two entries: name and numeric IP address; port is the advertised Ivy TCP
 * service port (zero before the handshake). No DNS lookup. Errors clear both
 * outputs. Use a fresh/freed snapshot, as for the other copies. */
int IvyContextCopyApplicationInfoInternal(IvyContext *ctx, IvyClientPtr peer,
    IvyStringSnapshot *snapshot, unsigned short *port);
/* Socket-side part of the private snapshot bridge. */
int IvySocketCopyPeerAddressInternal(Client client, char *address, size_t size);
int IvyContextCopyApplicationsInternal(IvyContext *ctx, IvyStringSnapshot *snapshot);
int IvyContextCopyApplicationRegexpsInternal(IvyContext *ctx, IvyClientPtr peer,
    IvyStringSnapshot *snapshot);
void IvyStringSnapshotFreeInternal(IvyStringSnapshot *snapshot);

#ifdef __cplusplus
}
#endif
