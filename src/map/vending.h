// Copyright (c) Hercules Dev Team, licensed under GNU GPL.
// See the LICENSE file
// Portions Copyright (c) Athena Dev Teams

#ifndef MAP_VENDING_H
#define MAP_VENDING_H

#include "common/hercules.h"

struct map_session_data;
struct s_search_store_search;
struct STORE_ITEM;

struct vending_interface_p;
struct vending_interface {
	struct vending_interface_p *p;

	void (*init) (bool minimal);
	void (*final) (void);

	void (*close) (struct map_session_data *sd, bool quitting);
	bool (*open) (struct map_session_data* sd, const char* message, const struct STORE_ITEM *data, int count);
	void (*list) (struct map_session_data* sd, unsigned int id);
	void (*purchase) (struct map_session_data* sd, int aid, unsigned int uid, const uint8* data, int count);
	bool (*search) (struct map_session_data* sd, unsigned short nameid);
	bool (*searchall) (struct map_session_data* sd, const struct s_search_store_search* s);
	struct DBIterator *(*iterator) (void);
};

#ifdef HERCULES_CORE
void vending_defaults(void);
#endif // HERCULES_CORE

HPShared struct vending_interface *vending;

#endif /* MAP_VENDING_H */
