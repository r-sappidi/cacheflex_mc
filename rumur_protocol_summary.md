# Rumur Coherence Protocol Summary

This repository contains a Rumur/Murphi model of the SPM coherence protocol
described by `spm_coherency_cc.csv` and `spm_coherency_dir.csv`.

The model is an explicit protocol model, not an atomic restatement of the CSV
tables. It models one cache line, three cache controllers, one directory, and a
finite unordered network with nondeterministic message delivery.

## What The Model Implements

The cache-controller CSV is encoded as the `CCState` state machine. It includes
the stable coherent states `I`, `S`, `E`, and `M`; transient request states such
as `IS^D`, `IM^AD`, `IM^A`, `SM^AD`, and `SM^A`; replacement/writeback states;
SPM migration states such as `SX^L`, `MX^L`, `EX^L`, and their ack-waiting
forms; and the SPM-resident state `X`.

The directory CSV is encoded as the `DirState` state machine. It includes
`I`, `S`, `E`, `M`, and `SD`. The model tracks the directory owner, the sharer
set, and a single SD transaction using an MSHR-like record containing the
requester, previous owner, and whether the request was silent.

Messages are modeled explicitly. The network can contain requests, forwarded
requests, invalidations, invalidation acknowledgements, put acknowledgements,
directory data, exclusive directory data, owner data, and owner-to-directory
data. This lets the model explore races where messages are delivered in
different orders.

Processor-side rules implement local cache actions:

- Loads from `I` send `GetS` and wait in `IS^D`.
- SPM coherent fetches send `GetS_silent` and wait in `II^D`.
- Store misses and upgrades send `GetM` and wait for data and/or invalidation
  acknowledgements.
- Store hits in `E` silently become `M`.
- SPM installs either move directly from `I` to `X`, or begin lazy migration
  from `S`, `E`, or `M`.
- Lazy migration may succeed locally and install SPM data, or fail and send a
  `PutS`, `PutE`, or `PutM` before entering an ack-waiting SPM transition.
- Replacements send the appropriate put message and wait for `PutAck`.
- `SPM_release` moves `X` back to `I`.

Directory rules implement request handling:

- In `I`, `GetS` grants exclusive data and records the requester as owner;
  `GetM` grants writable data and records the requester as owner;
  `GetS_silent` returns data without creating a directory participant.
- In `S`, `GetS` adds a sharer, while `GetS_silent` returns data without adding
  a sharer. `GetM` sends invalidations to the other sharers, clears the sharer
  set, records the requester as owner, and sends data with an ack count.
- In `E` or `M`, `GetS` and `GetS_silent` are forwarded to the current owner
  and move the directory to `SD`. Normal `GetS` adds the requester as a future
  sharer; silent `GetS` does not.
- In `E` or `M`, `GetM` is forwarded to the current owner, and ownership is
  transferred to the requester.
- `PutS`, `PutM`, and `PutE` remove sharers, clear ownership when appropriate,
  acknowledge the sender, and may complete an outstanding `SD` transaction.
- `SD` completes when owner data or a matching writeback arrives.

Controller receive rules implement the non-atomic events from the tables:

- Directory or owner data moves waiting request states to `S`, `E`, `M`, or `I`
  depending on the original request.
- Invalidations from `S` or related transient shared states send `InvAck` and
  invalidate or complete the SPM transition.
- Forwarded `GetS` from an `E` or `M` owner sends data to the requester and data
  to the directory, then downgrades to `S`.
- Forwarded `GetM` from an `E` or `M` owner sends data to the requester and
  invalidates the owner.
- `PutAck` completes replacement or failed lazy-SPM migration.
- Invalidation acknowledgements are counted until a pending `GetM` can become
  `M`.

## Checked Safety Properties

The model checks coherence-style safety invariants:

- At most one stable coherent writer exists.
- A stable coherent reader cannot coexist with another stable writer.
- SPM copies in `X` are not directory owners or sharers.
- Directory sharers correspond to plausible stable or transient controller
  states.
- A valid directory owner corresponds to a coherent or pending owner.
- An invalid directory has no stable coherent participants.
- `E` and `M` directory states have no sharers.
- `SD` always has an outstanding MSHR, and non-`SD` states do not.
- Ack counters are only used by pending `GetM` states.

## Assumptions Beyond The CSV Tables

The CSV tables specify local reactions, but they do not fully define the global
execution model. The Rumur model adds the following assumptions:

- The protocol is modeled for one cache line only.
- There are exactly three cache controllers.
- The network is unordered, finite, and nondeterministic.
- Message payload values are abstracted away; the model checks permissions and
  ownership, not data-value correctness.
- Directory `SD` has one outstanding transaction at a time.
- SPM state `X` is outside the coherence domain and must not appear in the
  directory sharer set or owner field.
- Silent `GetS`/SPM fetches receive data but do not become sharers.
- Lazy SPM migration is modeled nondeterministically as either success or
  failure. The model does not represent the physical way allocation details.
- Successful lazy SPM migration directly removes the modeled coherent
  participant and moves the requester to `X`.
- Failed lazy SPM migration is represented as a put/writeback followed by
  `PutAck`, after which the controller enters `X`.
- The model adds guards to avoid starting local actions while relevant forwarded
  requests, invalidations, pending data, or directory completions are already in
  flight.
- Stale forwarded requests and stale data responses may be dropped when they no
  longer match the current directory/controller context.
- Ack counting is made explicit. Invalidation acknowledgements may arrive before
  or after data, and the model tracks the remaining count.
- The focus is safety, not liveness. The invariants rule out illegal coherence
  states, but the model does not by itself prove progress or fairness.

