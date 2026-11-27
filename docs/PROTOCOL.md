# Order-entry protocol

OUCH-style messages inside SoupBinTCP-style framing. All integers are big-endian. Prices are integer ticks. Market data uses project 1's MoldUDP64-style protocol unchanged (see that repo's `docs/PROTOCOL.md`); the recovery protocol is also project 1's.

## Framing

```
+--------+--------+------------------+
| len u16| type u8| payload (len-1)  |
+--------+--------+------------------+
```

`len` covers type + payload; a frame is at most 64 bytes.

| Type | Direction | Payload |
|---|---|---|
| `L` Login Request | client → server | user[6] pass[10] requested_seq u64 (24 B) |
| `A` Login Accepted | server → client | session[10] first_seq u64 end_seq u64 (26 B) |
| `J` Login Rejected | server → client | reason u8 |
| `U` Unsequenced Data | client → server | one order message |
| `S` Sequenced Data | server → client | one order response; implicitly numbered 1, 2, 3… |
| `R` Client Heartbeat | client → server | none |
| `H` Server Heartbeat | server → client | none |
| `O` Logout | client → server | none |
| `Z` End of Session | server → client | none |

### Session rules

- **Login.** `requested_seq` is the next sequenced message the client expects (1 for a fresh session). The server replies with `first_seq` (where it will start sending) and `end_seq` (one past the last message already in its log). It then replays `[first_seq, end_seq)` and continues live. The client is ready once it has received up to `end_seq`.
- **Heartbeats.** Either side sends a heartbeat after 1 s without sending anything. 3 s without receiving anything means the connection is dead.
- **No resend.** The client never resends an order. After a reconnect, every order still unacknowledged when the replay has caught up is marked Lost (see DESIGN.md, decision 9).
- **Duplicates.** The server rejects a reused token with reason `T`, but the client never reuses one.

## Order messages (inside `U`)

**Enter Order `O`**: 21 bytes, a 24-byte frame on the wire.

| Offset | Field | Size |
|---|---|---|
| 0 | type `O` | 1 |
| 1 | token (client order id, unique per session) | 8 |
| 9 | side `B` / `S` | 1 |
| 10 | quantity | 4 |
| 14 | symbol id | 2 |
| 16 | price (ticks) | 4 |
| 20 | time in force `I` (IOC) / `D` (day; the simulator rejects it) | 1 |

The engine pre-builds this frame for every (symbol, side) and patches token, quantity and price in place (frame offsets 4, 13, 19).

**Cancel Order `X`**: 13 bytes: type, token u64, quantity u32 (0 = all).

## Order responses (inside `S`)

All start with type u8, timestamp u64 (exchange ns), token u64.

| Type | Extra fields | Total |
|---|---|---|
| `A` Accepted | side u8, qty u32, symbol u16, price u32, order ref u64 | 36 B |
| `E` Executed | qty u32, price u32, match id u64 | 33 B |
| `C` Canceled | qty u32, reason u8 (`I` IOC remainder, `U` user, `D` disconnect) | 22 B |
| `J` Rejected | reason u8 (`T` duplicate token, `S` bad symbol, `Q` bad qty, `X` unsupported) | 18 B |

An IOC produces `A`, then `E` if it crossed the book, then `C` for any remainder.

## Journal records (engine → control → file)

Fixed 64-byte records (`src/shm/layout.h`, field meanings in `src/engine/journal.h`):

| Record | Fields |
|---|---|
| RunStart | strategy params, symbols |
| Market | feed seq + full message |
| OrderSent | token, intent, send ok |
| OrderEvent | any response, plus the local `L` Lost verdict |
| RiskReject | reason, intent |
| Gating | enabled, reason bits |
| Limits | one per symbol per version |
| ControlApplied | version, kill |
| RunEnd | counts, drops |
