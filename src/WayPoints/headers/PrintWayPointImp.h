//
//  Copyright 2012 Alin Dobra and Christopher Jermaine
//
//  Licensed under the Apache License, Version 2.0 (the "License");
//  you may not use this file except in compliance with the License.
//  You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
//  Unless required by applicable law or agreed to in writing, software
//  distributed under the License is distributed on an "AS IS" BASIS,
//  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
//  See the License for the specific language governing permissions and
//  limitations under the License.
//
#ifndef PRINT_WP_IMP
#define PRINT_WP_IMP

#include "WayPointImp.h"
#include "WorkDescription.h"
#include <cstdio>

class PrintWayPointImp : public WayPointImp {
    private:
        QueryToFileMap streams;
	QueryToCounters counters;

        QueryExitContainer queriesToFinalize;
    public:

        // const and destruct
        PrintWayPointImp ();
        virtual ~PrintWayPointImp ();

        // here are the four funcs over-written by print
        void TypeSpecificConfigure (WayPointConfigureData &configData);
        void ProcessHoppingDownstreamMsg (HoppingDownstreamMsg &message);
        virtual void RequestGranted( GenericWorkToken & token ) override;
        void DoneProducing (QueryExitContainer &whichOnes, HistoryList &history, int result, ExecEngineData& data);
        void ProcessHoppingDataMsg (HoppingDataMsg &data);
};

#endif
