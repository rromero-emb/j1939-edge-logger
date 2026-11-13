# Design notes

## Why our own transport-protocol reassembly

Linux has a J1939 socket family (`CAN_J1939`, since 5.4) that handles the transport protocol. A logger running on top of a camera or gateway vendor's SDK often gets **raw CAN frames from a vendor plug-in API** instead of a socket, on a kernel built without that module. The reassembler here only needs frames with timestamps, so it runs on SocketCAN, on a vendor callback, or on a recorded trace alike. It is also passive: a kernel J1939 socket that is not the destination does not take part in someone else's RTS/CTS session, while a logger wants to see those transfers (for example the VIN sent to a diagnostic tool).

## Never disturbing the bus

- The interface goes up **listen-only**: the controller sends no ACK and no error frames.
- The bitrate is detected in listen-only mode. At the wrong rate the controller only sees errors; without ACKs from us nothing on the bus notices.
- The application has a single code path that builds a frame to send (`RequestGate`), off by default. Sending also needs a normal (not listen-only) interface and a claimed source address, so it is three separate decisions away from happening by accident.

## Journal format and power loss

```
record = magic u32 | length u32 | seq u64 | t_us u64 | payload[length] | crc32 u32
```

- One `write()` per record into a file opened with `O_APPEND`. A power cut can leave a partial record (short file) or a record with wrong bytes (page not fully programmed). Both fail the magic, length or CRC check at recovery, and the file is truncated there.
- Sequence numbers must be contiguous inside a segment, so stale bytes from an older layout can never be read as a valid continuation.
- `fdatasync` every `sync_every` records (default 32): on a 1 Hz signal set that is about 8 s of data at risk. Use 1 when every record matters more than flash wear.
- A new segment's directory entry is synced, otherwise a power cut could lose the whole file after its data was synced.
- The ack cursor is written to a temporary file and renamed. A torn ack file reads as "nothing acknowledged": the phone receives duplicates, never a gap.
- On raw NAND (UBIFS) or eMMC with ext4 `data=ordered` these guarantees hold. On FAT on an SD card they do not: a power cut can corrupt the FAT itself. Use ext4/f2fs, or a raw partition.

## BLE delivery (planned)

GATT service with three characteristics:
- **Records** (notify): batches from `SyncSession::next_batch`, fragmented to the negotiated MTU, each carrying `seq` of its first record.
- **Ack** (write): the phone writes the last `seq` it has stored durably; `SyncSession::ack` persists it.
- **State** (read/notify): the latest signals with their age, for a live dashboard.

On every new connection the session restarts from the durable ack, so the phone never has to ask for a replay explicitly.

## What a vendor SDK changes

Only the edges: frame input (SDK callback instead of `recvmsg`), the storage path, and the BLE stack the SDK exposes. The pipeline, the reassembly, the journal and the delivery logic do not depend on them, which is why they are tested on a PC.
