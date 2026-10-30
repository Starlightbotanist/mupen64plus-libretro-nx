#ifndef UCODE_SNAPSHOT_STATE_H
#define UCODE_SNAPSHOT_STATE_H

#include <cstddef>
#include <type_traits>

/* Same-instance continuation only. Command tables are rebuilt by GBI; these
 * blocks hold decoder-local values that can survive a display-list boundary. */
struct UcodeSnapshotBlock
{
	void *data;
	std::size_t size;
};

template <typename T>
UcodeSnapshotBlock ucodeSnapshotBlock(T & _state)
{
	static_assert(std::is_trivially_copyable<T>::value, "Snapshot state must not own resources");
	return { &_state, sizeof(_state) };
}

UcodeSnapshotBlock S2DEX_SnapshotState();
UcodeSnapshotBlock F5Rogue_SnapshotState();
UcodeSnapshotBlock T3DUX_SnapshotState();
UcodeSnapshotBlock ZSortBOSS_SnapshotState();
UcodeSnapshotBlock F5Indi_SnapshotState();
UcodeSnapshotBlock F5Naboo_SnapshotState();

#endif
