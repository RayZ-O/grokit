//
//  Copyright 2016 Rui Zhang
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
#ifndef _CONGESTIONCONTROLLER_H
#define _CONGESTIONCONTROLLER_H

#include <unordered_map>
#include <deque>
#include <cstdint>
#include <chrono>

// This class collects statistics of chunk processing and calculates the ideal delay
// time for next chunk to be produced
struct ChunkProcessStats;

class CongestionController {
private:
    // store the mapping between chunk id and chunk start time
    std::unordered_map<int, uint64_t> idToStartTime;
    // the underlying double ended queue to calculate running average
    std::deque<ChunkProcessStats> window;
    // size of the sliding window, the delay calculation algorithm only considers
    // the most recently finished chunks in the sliding window
    int windowSize;
    // number of drops in the sliding window
    int numDrops;
    // running sum of processing time in the window
    uint64_t runningSum;
    // remove first(oldest) stats in the window
    void RemoveFirst();
public:
    CongestionController(int wSize);
    // record when the chunk is produced
    void RecordChunkStart(int chunkID);
    // the 2 functions below update the statistics
    // when a drop is received, update number of drops and insert stats to window
    void ProcessDropMsg(int chunkID);
    // when an ack is received, update ideal delay time and insert stats to window
    void ProcessAckMsg(int chunkID);
    // return the ideal delay time to produce the next chunk
    int GetIdealDelayMillis();
};

struct ChunkProcessStats {
    uint64_t processingTime;  // time used to process this chunk in millisecond
    bool dropped;             // is this chunk dropped
    ChunkProcessStats(uint64_t _processingTime, bool _dropped) : processingTime(_processingTime),
                                                                 dropped(_dropped) { }
};

#endif
