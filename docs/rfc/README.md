# RFCs

A request for comments is a short design note for a renderer change that affects more than one game or
is hard to undo: a new render pass, a uniform or G-buffer layout change, a new subsystem (terrain), or
anything breaking. Small additive changes do not need one; an issue is enough (see
[`COORDINATION.md`](../../COORDINATION.md)).

## Process

1. Copy [`0000-template.md`](0000-template.md) to `NNNN-short-title.md` with the next free number.
   Any session may draft one; the game that needs the feature usually writes the first version (or
   points at its own design note) and the coordinator turns it into the RFC.
2. Open a GitHub issue that links it, titled `RFC NNNN: <title>`, and name every game it affects.
3. Each affected game's session comments on the issue (agree, or what it needs changed).
4. The coordinator sets the status to **Accepted** and merges the RFC; the implementation follows on a
   branch, opt-in, with a CHANGELOG entry and a release.
5. When it ships, the status becomes **Implemented in vX.Y.Z**. Rejected RFCs stay, marked **Rejected**,
   with the reason.

## Index

| RFC | Title | Status | Requested by |
| --- | --- | --- | --- |
| 0001 | Layered terrain shader | Waiting for CHIMERA's design note (`RTS/docs/v2/izgled-mapa.md`) | CHIMERA |
