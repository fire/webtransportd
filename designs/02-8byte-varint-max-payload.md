# Design: 8-byte Varint Encode & Configurable Max Payload

## Goal
Mirror 8-byte varint decode on the encoder and allow `WTD_FRAME_MAX_PAYLOAD` to be configurable above $2^{30}$.

## Steps
- Implement 8-byte varint encoding (encoder mirrors decoder logic from Cycle 25).
- Allow `WTD_FRAME_MAX_PAYLOAD` to be set above $2^{30}$.
- Add a behavioral test: encode a $2^{30}$-byte payload via a synthetic wrapper (no 1GB buffer needed).
- Ensure large reliable payloads do not force session close.
