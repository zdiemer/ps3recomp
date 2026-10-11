/*
 * ps3recomp - cellOvis SPU overlay staging
 *
 * Retail ELF staging APIs are implemented; legacy convenience APIs are stubs.
 */

#ifndef PS3RECOMP_CELL_OVIS_H
#define PS3RECOMP_CELL_OVIS_H

#include "ps3emu/ps3types.h"
#include "ps3emu/error_codes.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Retail overlay error codes. Legacy convenience APIs below are separate. */
#define CELL_OVIS_ERROR_INVAL 0x80410402
#define CELL_OVIS_ERROR_ABORT 0x8041040C
#define CELL_OVIS_ERROR_ALIGN 0x80410416
#define CELL_OVIS_ERROR_NOT_INITIALIZED     0x80410701
#define CELL_OVIS_ERROR_ALREADY_INITIALIZED 0x80410702
#define CELL_OVIS_ERROR_INVALID_ARGUMENT    0x80410703

/* Types */
typedef u32 CellOvisHandle;

/* Functions */
s32 cellOvisInit(void);
s32 cellOvisTerm(void);

s32 cellOvisGetOverlayTableSize(u32 elf);
s32 cellOvisInitializeOverlayTable(u32 table, u32 elf);
s32 cellOvisCreateOverlay(const void* table, u32 tableSize, CellOvisHandle* handle);
s32 cellOvisDestroyOverlay(CellOvisHandle handle);

s32 cellOvisInvalidateOverlay(CellOvisHandle handle);

#ifdef __cplusplus
}
#endif

#endif /* PS3RECOMP_CELL_OVIS_H */
