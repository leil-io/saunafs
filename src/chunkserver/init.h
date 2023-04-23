/*
   Copyright 2005-2010 Jakub Kruszona-Zawadzki, Gemius SA
   Copyright 2013-2014 EditShare
   Copyright 2013-2015 Skytechnology sp. z o.o.
   Copyright 2023      Leil Storage OÜ


   SaunaFS is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, version 3.

   SaunaFS is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with SaunaFS  If not, see <http://www.gnu.org/licenses/>.
 */

#include "common/platform.h"

#include <stdio.h>

#include "chunkserver/chartsdata.h"
#include "chunkserver/hddspacemgr.h"
#include "chunkserver/masterconn.h"
#include "chunkserver/network_main_thread.h"
#include "common/random.h"

#define STR_AUX(x) #x
#define STR(x) STR_AUX(x)

/* Run Tab */
typedef int (*runfn)(void);
struct run_tab {
	runfn fn;
	const char *name;
};

run_tab RunTab[] = {
    {rnd_init, "random generator"},
    {loadPlugins, "plugin manager"},
    {hddInit, "hdd space manager"},
    {mainNetworkThreadInit,
     "main server module"}, /* it has to be before "masterconn" */
    {masterconn_init, "master connection module"},
    {chartsdata_init, "charts module"},
    {(runfn)0, "****"}};

run_tab LateRunTab[] = {
    {masterconn_init_threads, "master connection module - threads"},
    {hddLateInit, "hdd space manager - threads"},
    {mainNetworkThreadInitThreads, "main server module - threads"},
    {(runfn)0, "****"}};

run_tab EarlyRunTab[] = {{(runfn)0, "****"}};
