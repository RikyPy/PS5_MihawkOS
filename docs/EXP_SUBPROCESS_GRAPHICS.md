# Subprocess graphics experiment: disposition

Reviewed 2026-10-07. This experiment led to the **working shared GPU broker**,
now used by production payload clients. Its original hypotheses and rejected
routes are preserved in [the experiment history](history/2026-10-07/docs/EXP_SUBPROCESS_GRAPHICS.md).

## Conclusion

Direct native GPU initialization in a Sony-managed helper failed in the tested
setup. Reusing the original title for one Windows graphics client worked but
limited the title to one client per session. Neither result established that
multiple graphical Windows processes were impossible.

The adopted route retains one native GPU/display owner and brokers GPU work for
separate clients. Native Vulkan probes, owner presentation and Windows/DXVK
children/descendants passed. Payload spawning then removed the previous Sony
helper ceiling. Mesa/Zink now supplies the default helper OpenGL route.

## Read next

- [GRAPHICS_PROCESS_BROKER.md](GRAPHICS_PROCESS_BROKER.md): current broker and ownership.
- [WINDOWS_PROCESSES.md](WINDOWS_PROCESSES.md): payload, memory and application results.
- [ARCHITECTURE.md](ARCHITECTURE.md): full boot/teardown contract.
- [PLAN.md](PLAN.md): application acceptance and remaining qualification.

The archived proposals are not instructions to rerun direct-GPU or rfork probes.
Eight-child and four-renderer results are separate; arbitrary concurrent games
and all graphics APIs remain compatibility work.
