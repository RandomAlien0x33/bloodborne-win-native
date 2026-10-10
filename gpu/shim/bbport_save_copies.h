/* bbport: copies of the save folders (src/runtime_savecopies.c), used by the menus. C and C++. */
#ifndef BBPORT_SAVE_COPIES_H
#define BBPORT_SAVE_COPIES_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char name[48]; /* folder name: YYYYMMDD-HHMMSS-manual / -auto */
    char when[32]; /* YYYY-MM-DD HH:MM:SS */
    int manual;    /* made by the player (else automatic: at start, before a load) */
    unsigned place; /* Bloodborne: where the character was (PlaceName text id, as the game's
                       Load Game screen shows it); 0: unknown */
} RuntimeSaveCopy;

/* Copies the save folders now; the oldest copies of the same kind beyond `keep` are removed.
 * Waits up to 3 s for a save in progress. 0 on success; the new copy's name in name_out. 1: the save
 * is the same as in the newest copy of that kind (the game has not saved since): no new copy, that
 * one's name in name_out. */
int runtime_saves_copy(int manual, int keep, char *name_out, size_t name_size);
/* The save as it is now (the game's own): the place of the character played and when it was saved
 * last (YYYY-MM-DD HH:MM:SS). 0 when there is one. */
int runtime_saves_current(unsigned *place, char *when, size_t when_size);
/* Copies, newest first. */
int runtime_saves_list(RuntimeSaveCopy *out, int max);
/* Starts loading copy `name` (empty: the game's own save as it is on disk): copies the current
 * state (automatic) and freezes the save files; the caller sends the game to the title screen.
 * The copy goes in when the title screen mounts the save. 0 on success. */
int runtime_saves_begin_load(const char *name);
/* A load is under way (until the title screen reads the save). */
int runtime_saves_loading(void);
/* Gives a load up that has not reached the title screen: save writes back on. 1 when one was under way. */
int runtime_saves_cancel_load(void);
/* runtime_savedata.c: the game mounted its save (read_only: as the title screen does). */
void runtime_saves_mounted(int read_only);
/* At start: removes copies a crash interrupted. */
void runtime_saves_startup(const char *title_id);

#ifdef __cplusplus
}
#endif
#endif
