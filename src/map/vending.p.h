// Copyright (c) Hercules Dev Team, licensed under GNU GPL.
// See the LICENSE file
// Base author: Haru <haru@dotalux.com>

#ifndef MAP_VENDING_P_H
#define MAP_VENDING_P_H

#include "vending.h"

#include "common/hercules.h"
#include "common/db.h"

struct vending_interface_p {
	uint32 next_id; ///< next vender id
	DBMap *db;

	uint32 (*getid) (void);
};

#endif // MAP_VENDING_P_H
