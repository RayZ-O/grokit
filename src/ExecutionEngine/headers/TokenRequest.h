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

#ifndef _TOKENREQUEST_H_
#define _TOKENREQUEST_H_

#include "WayPointID.h"

// this is a silly little struct that is used to hold requests for resource tokens
struct TokenRequest {

    WayPointID whoIsAsking;
    int priority;

    TokenRequest () {}
    virtual ~TokenRequest () {}

    TokenRequest (WayPointID whoIn, int priorityIn) {
        whoIsAsking = whoIn;
        priority = priorityIn;
    }
    // delete copy constructor and copy assignment operator
    TokenRequest(const TokenRequest &copyMe) = delete;
    TokenRequest& operator = (const TokenRequest &copyMe) = delete;

    // default move constructor and move assignment operator
    TokenRequest(TokenRequest &&moveMe) = default;
    TokenRequest& operator = (TokenRequest &&moveMe) = default;

    void swap (TokenRequest &withMe) {
        char temp[sizeof (TokenRequest)];
        memmove (temp, &withMe, sizeof (TokenRequest));
        memmove (&withMe, this, sizeof (TokenRequest));
        memmove (this, temp, sizeof (TokenRequest));
    }
};

struct DelayTokenRequest : public TokenRequest {
    // the 2 fields below are for delay token, Unix timestamps should have at lease 55 bits
    uint64_t insertedTimeMillis;  // when is this request made (Unix epoch)
    uint64_t expectedTimeMillis; // when is this request expected to be granted (Unix epoch)

    DelayTokenRequest () {}
    ~DelayTokenRequest () {}

    // constructor for delay token
    DelayTokenRequest (WayPointID whoIn, int priorityIn, uint64_t millis) : TokenRequest(whoIn, priorityIn) {
        auto now = std::chrono::system_clock::now().time_since_epoch();
        insertedTimeMillis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
        expectedTimeMillis = insertedTimeMillis + millis;
    }

    // delete copy constructor and copy assignment operator
    DelayTokenRequest(const DelayTokenRequest& copyMe) = delete;
    DelayTokenRequest& operator = (const DelayTokenRequest& copyMe) = delete;

    // default move constructor and move assignment operator
    DelayTokenRequest(DelayTokenRequest &&moveMe) = default;
    DelayTokenRequest& operator = (DelayTokenRequest &&moveMe) = default;

    void swap (DelayTokenRequest &withMe) {
        char temp[sizeof (DelayTokenRequest)];
        memmove (temp, &withMe, sizeof (DelayTokenRequest));
        memmove (&withMe, this, sizeof (DelayTokenRequest));
        memmove (this, temp, sizeof (DelayTokenRequest));
    }
};

// comparator for the delay token priority queue
class DelayTokenRequestComparator {
public:
    bool operator() (const DelayTokenRequest &token1, const DelayTokenRequest &token2) {
        // the request with the smallest expected time should be granted first
        if (token1.expectedTimeMillis != token2.expectedTimeMillis) {
            return token1.expectedTimeMillis > token2.expectedTimeMillis;
        }
        // if the expected time are equal, compare the inserted time
        if (token1.insertedTimeMillis != token2.insertedTimeMillis) {
            return token1.insertedTimeMillis > token2.insertedTimeMillis;
        }

        return true;
    }
};

#endif
