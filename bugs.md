**Bug Summary: Incorrect Stream Pointer Returned by `udpCreate` Leading to `udpBfrReturn` Failure (-23013)**

**1. Issue Summary:**

When attempting to use MacTCP's User Datagram Protocol (UDP) via low-level `PBControlSync` calls within an application compiled using the Retro68 GCC toolchain, the `udpCreate` control call successfully returns `noErr`. However, the `StreamPtr` value written back into the `udpStream` field of the `UDPiopb` parameter block appears to be incorrect. Instead of returning a pointer to MacTCP's internal stream control block, it returns the memory address of the user-provided receive buffer (`rcvBuff`).

While this non-zero value might be sufficient for a later `udpRelease` call to succeed (possibly because `udpRelease` also receives the buffer pointer explicitly), it causes subsequent `udpBfrReturn` calls (which are necessary after successfully reading data with `udpRead`) to fail with error `-23013` (`invalidBufPtr`). MacTCP interprets this error as the provided buffer pointer not belonging to the stream identified by the (incorrect) stream pointer, leading to a failure to return the buffer to MacTCP's pool. This ultimately prevents reliable, continuous UDP reception, as MacTCP will eventually exhaust its internal references to the application's receive buffer segments.

**2. Background & Context:**

*   **Environment:** Classic Macintosh (System 7.5.3 on Performa 6200), MacTCP v2.0.6.
*   **Development Toolchain:** Retro68 (GCC-based cross-compiler for m68k Macintosh).
*   **API:** MacTCP Driver accessed via low-level Device Manager calls (`PBOpenSync`, `PBControlSync`, `PBCloseSync`).
*   **Interface:** Using the `UDPiopb` parameter block structure defined in `<MacTCP.h>` (or equivalent Universal Headers) for UDP control calls (`udpCreate`, `udpRead`, `udpWrite`, `udpBfrReturn`, `udpRelease`).
*   **Goal:** Implement UDP broadcast discovery and peer-to-peer communication.

**3. Symptoms & Debugging Progression:**

*   **Initial State (No special alignment flags):**
    *   `udpCreate` call using `PBControlSync` returned `noErr`.
    *   However, inspecting the `pb.udpStream` field *after* the call revealed it was `0L` (NULL).
    *   Subsequent calls like `udpRead` or `udpWrite` would fail immediately or behave unpredictably because they require a valid `StreamPtr`.
    *   Attempts to call `udpBfrReturn` or `udpRelease` with a NULL `StreamPtr` would fail with `invalidStreamPtr` (-23010).
*   **With `-fpack-struct` Compiler Flag:**
    *   The Retro68 GCC compiler was instructed to pack structure members tightly, removing default alignment padding.
    *   `udpCreate` call using `PBControlSync` still returned `noErr`.
    *   Inspecting `pb.udpStream` *after* the call now showed a **non-zero** value. Crucially, this value was observed to be the **exact memory address** of the `rcvBuff` pointer (`gUDPRecvBuffer`) that was passed *into* the `udpCreate` call.
    *   `udpRead` calls now successfully returned `noErr` when packets arrived, indicating MacTCP could associate incoming data with the stream (identified somehow, perhaps via the buffer address).
    *   **NEW FAILURE:** The `udpBfrReturn` call, made after a successful `udpRead` that received data, now consistently failed with error `-23013` (`invalidBufPtr`).
    *   The `udpRelease` call during cleanup *succeeded* when passed the buffer address as the `udpStream`.

**4. Root Cause Analysis (The "Why"):**

The most likely cause is a **structure alignment mismatch** between the application code compiled by Retro68 GCC (even with `-fpack-struct`) and the pre-compiled MacTCP driver code (originally compiled with MPW C).

*   **Structure Layout:** MacTCP expects the `UDPiopb` parameter block passed via `PBControlSync` to have a precise memory layout, determined by the MPW C compiler's rules at the time MacTCP was built. Modern GCC has different default alignment rules.
*   **`-fpack-struct` Effect:** This flag forces GCC to minimize padding *between* structure members. This brought the layout *closer* to what MacTCP expects, allowing MacTCP to correctly read the *input* parameters like `csCode`, `rcvBuff`, and `rcvBuffLen` from their (now packed) locations. This is why `udpCreate` started returning `noErr`.
*   **The Output Problem:** However, when MacTCP needs to write the *output* `StreamPtr` back into the parameter block, it calculates the memory offset based on its *internal* (likely MPW C) understanding of the structure layout. Due to subtle remaining differences (perhaps alignment *within* the `csParam` union, or the alignment of the union itself relative to `udpStream`), the offset MacTCP calculates for `udpStream` doesn't correspond to the `udpStream` field in the Retro68 `-fpack-struct` layout. Instead, it appears to correspond exactly to the location of the `csParam.create.rcvBuff` field within the union.
*   **Result:** MacTCP writes the *intended* internal stream pointer value into the memory location it *thinks* is `udpStream`, but which is actually `csParam.create.rcvBuff` in the application's view. It might then write the *original value* it read from `csParam.create.rcvBuff` (the buffer address) into the memory location it *thinks* is `udpStream`. The application then reads `pb.udpStream` and gets the buffer address (`0x222D400` in your logs).
*   **`udpBfrReturn` Failure:** This call requires the *true* internal `StreamPtr` to correctly identify the stream and manage its buffer queue. When it receives the buffer address instead, it cannot find a matching internal stream control block and fails with `-23013` (effectively, "this buffer doesn't belong to the stream identified by *this* pointer").
*   **`udpRelease` Success:** This call might succeed because it receives *both* the (incorrect) stream pointer *and* the correct buffer pointer (`rcvBuff`) and length (`rcvBuffLen`) as explicit parameters within the `csParam.create` part of the union (as per the MacTCP guide). It might use the buffer pointer directly to find and release the associated resources, being more tolerant of the incorrect stream pointer value for this specific cleanup operation.

**5. Code Context (The "Where" and "How"):**

*   **Structure Definition (`<MacTCP.h>`):**

    ```c
    typedef struct UDPiopb {
        // ... standard header ... (28 bytes total)
        short             csCode;         // Offset 28
        StreamPtr         udpStream;      // Offset 30 <- PROBLEM AREA (Output for Create, Input for others)
        union {                         // Offset 34
            struct UDPCreatePB  create;   // Used by udpCreate
            struct UDPSendPB    send;     // Used by udpWrite
            struct UDPReceivePB receive;  // Used by udpRead, udpBfrReturn
            struct UDPMTUPB     mtu;
        } csParam;
    } UDPiopb;

    typedef struct UDPCreatePB {
        Ptr           rcvBuff;        // Offset 34 + 0 = 34 <- Value being returned in udpStream?
        unsigned long rcvBuffLen;     // Offset 34 + 4 = 38
        UDPNotifyProc notifyProc;     // Offset 34 + 8 = 42
        unsigned short localPort;     // Offset 34 + 12 = 46
        Ptr           userDataPtr;    // Offset 34 + 14 = 48 (Size = 18 bytes)
    } UDPCreatePB;

    typedef struct UDPReceivePB {
        // ... other fields ...
        Ptr            rcvBuff;       // Offset 34 + 8 = 42 <- INPUT for udpBfrReturn
        // ... other fields ...
    } UDPReceivePB;
    ```
    *(Note: Offsets calculated assuming packed layout, actual offsets might vary slightly based on compiler specifics)*

*   **`udpCreate` Call in `InitUDPDiscoveryEndpoint`:**

    ```c
    // ... setup pb ...
    pb.csParam.create.rcvBuff = gUDPRecvBuffer; // Input: 0x222D400
    pb.csParam.create.rcvBuffLen = kMinUDPBufSize;
    pb.udpStream = 0L; // Cleared before call

    err = PBControlSync((ParmBlkPtr)&pb);

    // After call:
    // err = 0 (noErr)
    // pb.udpStream = 0x222D400 (Problem: Contains rcvBuff address, not internal stream ptr)
    gUDPStream = pb.udpStream;
    ```

*   **`udpBfrReturn` Call in `CheckUDPReceive`:**

    ```c
    // After successful udpRead where bytesReceived > 0...
    memset(&bfrReturnPB, 0, sizeof(UDPiopb));
    // ... set common fields ...
    bfrReturnPB.csCode = udpBfrReturn;
    bfrReturnPB.udpStream = gUDPStream; // Input: Passes 0x222D400 (the buffer address)
    bfrReturnPB.csParam.receive.rcvBuff = gUDPRecvBuffer; // Input: Passes 0x222D400

    returnErr = PBControlSync((ParmBlkPtr)&bfrReturnPB);
    // returnErr = -23013 (invalidBufPtr - MacTCP doesn't associate stream '0x222D400' with buffer '0x222D400')
    ```

**6. Log Evidence:**

```
DEBUG: After PBControlSync(udpCreate): err=0, pb.udpStream=0x222D400, pb.csParam.create.localPort=8081
...
// (udpRead succeeds, receives data into gUDPRecvBuffer)
...
CRITICAL Error (UDP Receive): PBControlSync(udpBfrReturn) failed with stream 0x222D400. Error: -23013. UDP receive may stop working.
...
// (Cleanup)
Attempting PBControlSync (udpRelease) for endpoint 0x222D400...
PBControlSync(udpRelease) succeeded.
Disposing UDP receive buffer at 0x222D400.
```

**7. Impact:**

The inability to successfully call `udpBfrReturn` means that the memory segments within `gUDPRecvBuffer` used by MacTCP for incoming packets are never marked as available again for MacTCP's internal management of that stream. Eventually, MacTCP will believe it has no more buffer space available for this stream and will stop receiving UDP packets, likely leading to `udpRead` calls timing out or returning errors.

**8. Conclusion:**

The core problem is a persistent, subtle structure alignment mismatch between the Retro68/GCC build environment (even with `-fpack-struct`) and the expected layout of the `UDPiopb` by the pre-compiled MacTCP driver. This causes `udpCreate` to write the wrong value (the user buffer address) into the `udpStream` field, which subsequently breaks `udpBfrReturn`. Compiling with MPW C on a real Mac remains the best way to verify the expected structure layout and potentially resolve the issue, or alternatively, exploring higher-level UDP APIs if the low-level approach proves intractable due to these alignment problems.