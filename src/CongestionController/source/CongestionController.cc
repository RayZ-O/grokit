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

#include "CongestionController.h"

using namespace std;

CongestionController :: CongestionController(int wSize) : windowSize(wSize), numDrops(0), runningSum(0UL) { }

void CongestionController :: RecordChunkStart(int chunkID) {
    // get current time in millisecond
    auto now = chrono::system_clock::now().time_since_epoch();
    uint64_t nowInMillis = chrono::duration_cast<chrono::milliseconds>(now).count();

    idToStartTime.emplace(chunkID, nowInMillis);
}

void CongestionController :: ProcessDropMsg(int chunkID) {
    if (idToStartTime.find(chunkID) == idToStartTime.end()) {
        return;
    }
    // if the window is full, remove oldest element
    if (window.size() == windowSize) {
        RemoveFirst();
    }
    idToStartTime.erase(chunkID);
    // update number of drops and insert stats into window
    numDrops++;
    window.emplace_back(0, true);
}

void CongestionController :: ProcessAckMsg(int chunkID) {
    if (idToStartTime.find(chunkID) == idToStartTime.end()) {
        return;
    }
    // if the window is full, remove oldest element
    if (window.size() == windowSize) {
        RemoveFirst();
    }
    // get current time in millisecond
    auto now = chrono::system_clock::now().time_since_epoch();
    uint64_t nowInMillis = chrono::duration_cast<chrono::milliseconds>(now).count();
    // update the running sum for calculating the ideal delay time
    uint64_t processingTime = nowInMillis - idToStartTime[chunkID];
    runningSum += processingTime;

    idToStartTime.erase(chunkID);
    window.emplace_back(processingTime, false);
}

int CongestionController :: GetIdealDelayMillis() {
    if (window.empty() || static_cast<double>(numDrops) / window.size() < 0.05) {
        // if there is no available statistics or dropping rate is less than 5%
        // delay is not necessary
        return 0;
    } else {
        // otherwise the ideal delay time is the average processing time of chunks in the window
        return runningSum / window.size();
    }
}

void CongestionController :: Reset() {
    idToStartTime.clear();
    window.clear();

    numDrops = 0;
    runningSum = 0UL;
}

void CongestionController :: RemoveFirst() {
    if (window.empty()) {
        return;
    }
    if (window.front().dropped) {
        // if the chunk at the front is drop, update number of drops
        numDrops--;
    } else {
        // otherwise update running sum
        runningSum -= window.front().processingTime;
    }
    window.pop_front();
}
