#ifndef DIALOG_PEERLIST_H
#define DIALOG_PEERLIST_H

#include <MacTypes.h>
#include <Lists.h>     // For ListHandle, Cell
#include <Dialogs.h>   // For DialogPtr
#include <Events.h>    // For EventRecord
#include "peer_mac.h"  // For peer_t definition and MAX_PEERS

/*----------------------------------------------------------*/
/* External Globals (Defined in dialog.c or peer_mac.c)     */
/*----------------------------------------------------------*/

extern DialogPtr gMainWindow;       // The main dialog window
extern ListHandle gPeerListHandle;  // Handle to the List Manager list
extern Cell gLastSelectedCell;      // Tracks the last clicked/selected cell
extern peer_t gPeerList[MAX_PEERS]; // The actual peer data array

/*----------------------------------------------------------*/
/* Function Prototypes                                      */
/*----------------------------------------------------------*/

/**
 * @brief Initializes the Peer List Manager list control.
 *
 * Gets the dialog item userItem rectangle and creates the list using LNew.
 * Sets list properties like selection flags.
 *
 * @param dialog The parent dialog pointer.
 * @return Boolean True if initialization was successful, false otherwise.
 */
Boolean InitPeerListControl(DialogPtr dialog);

/**
 * @brief Cleans up resources used by the Peer List control.
 *
 * Disposes of the list using LDispose.
 */
void CleanupPeerListControl(void);

/**
 * @brief Handles mouse clicks within the Peer List's view rectangle.
 *
 * Checks if the click is within the list's bounds and calls LClick.
 * Updates gLastSelectedCell based on the selection state after the click.
 *
 * @param dialog The parent dialog pointer (used for coordinate conversion).
 * @param theEvent The mouse down event record.
 * @return Boolean True if the click was processed by LClick, false otherwise.
 */
Boolean HandlePeerListClick(DialogPtr dialog, EventRecord *theEvent);

/**
 * @brief Updates the rows displayed in the List Manager list based on gPeerList.
 *
 * Clears existing rows, iterates through active peers in gPeerList,
 * adds rows, sets cell contents, and restores/updates the selection.
 * Invalidates the list's view rectangle if changes occurred or forceRedraw is true.
 * Calls PruneTimedOutPeers internally.
 *
 * @param forceRedraw If true, always invalidate the list's view rectangle.
 */
void UpdatePeerDisplayList(Boolean forceRedraw);

/**
 * @brief Updates the display of the Peer List during an update event.
 *
 * Calls LUpdate to redraw the list within the window's visible region.
 *
 * @param dialog The parent dialog pointer (used to get the window port).
 */
void HandlePeerListUpdate(DialogPtr dialog);

/**
 * @brief Gets information about the currently selected peer in the list.
 *
 * Translates the selected cell row (gLastSelectedCell.v) into an index
 * within the active peers in gPeerList.
 *
 * @param outPeer Pointer to a peer_t structure to fill with the selected peer's data.
 * @return Boolean True if a peer is selected and its data was retrieved, false otherwise.
 */
Boolean GetSelectedPeerInfo(peer_t *outPeer);


#endif /* DIALOG_PEERLIST_H */