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

#include "ExecEngineImp.h"
#include "ExecEngine.h"
#include "Constants.h"
#include "CPUWorkerPool.h"
#include "Diagnose.h"
#include "DiskPool.h"
#include "CommunicationFramework.h"

using namespace std;

void ExecEngine :: Debugg () {
  ExecEngineImp *temp = dynamic_cast <ExecEngineImp *> (evProc);
  temp->Debugg ();
}

void ExecEngineImp :: Debugg(void){
    // just go through the list of waypoints and ask all of them to debug
    FOREACH_EM(key, data, myWayPoints){
        printf("Debugging WayPoint %s\n", key.getName().c_str());
        data.Debugg();
    }END_FOREACH
    printf(" \n ------- unused CPU token = %d",unusedCPUTokens.size());
    printf(" \n ------- unused Disk token = %d",unusedDiskTokens.size());
    printf(" \n ------- CPU request List = %d", requestListCPU.size());
    printf(" \n ------- Disk request List = %d", requestListDisk.size());
    printf(" \n ------- Num requests in list =%d", requests.size());

}

ExecEngineImp :: ~ExecEngineImp () {}


ExecEngineImp :: ExecEngineImp (const std::string & _mailbox) :
    // Explicitly initialize all member data structures to make sure they are all valid
    EventProcessorImp(),
    requests(),
    unusedCPUTokens(),
    unusedDiskTokens(),
    hoppingDataMessages(),
    hoppingDownstreamMessages(),
    acks(),
    drops(),
    directMessages(),
    myGraph(),
    myWayPoints(),
    requestListCPU(),
    requestListDisk(),
    frozenOutFromCPU(),
    frozenOutFromDisk(),
    delayRequestListCPU(),
    delayRequestListDisk(),
    // initially, anything can run... so we use a very large number as the cutoff
    priorityCPU(999),
    priorityDisk(999),
    holdMe(),
    // note that we are not currently holding a work token for reclamation
    holdMeIsValid(0),
    mailbox(_mailbox),
    serviceFrontend(),
    services()
{

    RegisterMessageProcessor (HoppingDataMsgMessage::type, &HoppingDataMsgReady, 1);
    RegisterMessageProcessor (ConfigureExecEngineMessage::type, &ConfigureExecEngine, 1);
    RegisterMessageProcessor (ServiceRequestMessage::type, &ServiceRequestMessage_H, 3);
    RegisterMessageProcessor (ServiceControlMessage::type, &ServiceControlMessage_H, 2);

    // create and load up all of the CPU tokens
    for (int i = 0; i < NUM_EXEC_ENGINE_THREADS; i++) {
        unusedCPUTokens.emplace_back(i + 100);
    }

    // create and load up all of the disk tokens
    for (int i = 0; i < NUM_DISK_TOKENS; i++) {
        unusedDiskTokens.emplace_back(i + 200);
    }
}

void ExecEngineImp :: PreStart(void) {
    // Register self with communication framework to receive messages for
    // services.
    EventProcessor self = Self();
    RegisterAsRemoteEventProcessor(self, mailbox);

    // Get proxy for actor in frontend to send service reply and info messages.
    HostAddress frontend;
    GetFrontendAddress(frontend);
    MailboxAddress serviceFrontendAddress(frontend, "grokit_services");
    FindRemoteEventProcessor(serviceFrontendAddress, serviceFrontend);
}

// this function picks one message/token and delivers it to the place it needs to go to next
int ExecEngineImp :: DeliverSomeMessage () {


    // first, see if there are any requests
    if (requests.empty())
        return 0;

    MessageType whatToDo = RemoveRequest();

    switch (whatToDo) {

        /************************/
        case MessageType::HOPPING_DOWNSTREAM_MESSAGE:
        {

            // take the message out
            HoppingDownstreamMsg &temp = hoppingDownstreamMessages.front();
            hoppingDownstreamMessages.pop_front();

            // now find all of the places it needs to be routed to
            InefficientMap <WayPointID, QueryExitContainer> allSubsets;
            myGraph.FindAllRoutings (temp.get_currentPos(), temp.get_dest(), allSubsets);

            // process all of the destinations
            allSubsets.MoveToStart ();
            while (!allSubsets.AtEnd ()) {

                // create the new hopping downstream message
                HoppingDownstreamMsg temp2;
                temp2.copy (temp);

                // put the destination query exits into it
                temp2.get_dest ().swap (allSubsets.CurrentData ());
                temp2.get_currentPos () = allSubsets.CurrentKey ();

                // find the waypoint it needs to be delivered to
                WayPointID myWayPointID;
                myWayPointID = allSubsets.CurrentKey ();
                WayPoint &myWayPoint = myWayPoints.Find (myWayPointID);

#ifdef DEBUG
                string QEs;
                temp2.get_dest ().MoveToStart();
                while (temp2.get_dest ().RightLength()) {
                    QEs += temp2.get_dest ().Current().GetStr();
                    QEs += " ";
                    temp2.get_dest ().Advance();
                }
#endif
                // deliver it
                temp2.get_currentPos ().swap (myWayPointID);
                PDEBUG("Sending DOWNSTREAM message of type %s to %s with current pos = %s, nextDest = %s,  and destination Query Exits = %s",
                        temp2.get_msg().TypeName(), myWayPoint.GetID().getName().c_str(), temp.get_currentPos ().GetStr().c_str(), temp2.get_currentPos ().GetStr().c_str(), QEs.c_str());
                DIAGNOSE_ENTRY("ExecutionEngine", myWayPoint.GetID().getName().c_str(), temp2.get_msg().TypeName());

                myWayPoint.ProcessHoppingDownstreamMsg (temp2);

                // and move on in the list of subsets
                allSubsets.Advance ();
            }

            // and get outta here!
            return 1;

            /****************************/
        } case MessageType::HOPPING_UPSTREAM_MESSAGE: {

            // take the message out
            HoppingUpstreamMsg &temp = hoppingUpstreamMessages.front();
            hoppingUpstreamMessages.pop_front();

            // now find the place it needs to be routed to
            WayPointIDContainer nextOnes;
            myGraph.FindUpstreamWaypoints (temp.get_currentPos (), temp.get_dest (), nextOnes);

            for (nextOnes.MoveToStart (); nextOnes.RightLength (); nextOnes.Advance ()) {

                // now, actually deliver the message
                HoppingUpstreamMsg temp2;
                temp2.copy (temp);
                temp2.get_currentPos () = nextOnes.Current ();
                WayPoint &myWayPoint = myWayPoints.Find (nextOnes.Current ());
                PDEBUG("Sending UPSTREAM message of type %s to %s with current pos = %s, nextDest = %s, and destination Query Exit = %s",
                        temp2.get_msg().TypeName(), myWayPoint.GetID().getName().c_str(), temp.get_currentPos ().GetStr().c_str(), temp2.get_currentPos ().GetStr().c_str(), temp2.get_dest().GetStr().c_str());
                DIAGNOSE_ENTRY("ExecutionEngine", myWayPoint.GetID().getName().c_str(), temp2.get_msg().TypeName());

                myWayPoint.ProcessHoppingUpstreamMsg (temp2);
            }

            // and get outta here!
            return 1;


            /*******/
        } case MessageType::ACK: {

            // take the message out
            LineageData &temp = acks.front();
            acks.pop_front();

            // now find the place it needs to be routed to
            temp.history.MoveToFinish ();
            FATALIF (!temp.history.LeftLength (), "Why do I have an empty HistoryList?");
            temp.history.Retreat ();

            // now, actually deliver the message
            WayPoint &myWayPoint = myWayPoints.Find (temp.history.Current ().get_whichWayPoint ());
            PDEBUG("Sending ACK message to %s", myWayPoint.GetID().getName().c_str());
            DIAGNOSE_ENTRY("ExecutionEngine", myWayPoint.GetID().getName().c_str(), "ACK");
            myWayPoint.ProcessAckMsg (temp.whichOnes, temp.history);

            // and get outta here!
            return 1;

            /********/
        } case MessageType::DROP: {

            // take the message out
            LineageData &temp = drops.front();
            drops.pop_front();

            // now find the place it needs to be routed to
            temp.history.MoveToFinish ();
            FATALIF (!temp.history.LeftLength (), "Why do I have an empty HistoryList?");
            temp.history.Retreat ();

            // now, actually deliver the message
            WayPoint &myWayPoint = myWayPoints.Find (temp.history.Current ().get_whichWayPoint ());
            PDEBUG("Sending DROP message to %s", myWayPoint.GetID().getName().c_str());
            DIAGNOSE_ENTRY("ExecutionEngine", myWayPoint.GetID().getName().c_str(), "DROP");
            myWayPoint.ProcessDropMsg (temp.whichOnes, temp.history);

            // and get outta here!
            return 1;

            /******************/
        } case MessageType::DIRECT_MESSAGE: {

            // send a direct message
            DirectMsg &temp = directMessages.front();
            directMessages.pop_front();

            // now, actually deliver the message
            WayPoint &myWayPoint = myWayPoints.Find (temp.get_receiver ());
            PDEBUG("Sending DIRECT message to %s", myWayPoint.GetID().getName().c_str());
            DIAGNOSE_ENTRY("ExecutionEngine", myWayPoint.GetID().getName().c_str(), "DIRECT");
            myWayPoint.ProcessDirectMsg (temp);

            // and get outta here!
            return 1;

            /************************/
        } case MessageType::HOPPING_DATA_MESSAGE: {

            // take the message out
            HoppingDataMsg &temp = hoppingDataMessages.front();
            hoppingDataMessages.pop_front();

            // now find all of the places it needs to be routed to
            InefficientMap <WayPointID, QueryExitContainer> allSubsets;

            myGraph.FindAllRoutings (temp.get_currentPos (), temp.get_dest (), allSubsets);
            allSubsets.MoveToStart ();
            if (allSubsets.AtEnd()){
                cout << temp.get_currentPos ().getName() << "\t";
                FOREACH_TWL(qe, temp.get_dest ()){
                    qe.Print();
                }END_FOREACH;
                WARNING("Data Message did not get delievered");
            }
            while (!allSubsets.AtEnd ()) {

                // create the new hopping data message
                HoppingDataMsg temp2;
                temp2.copy (temp);

                // find the waypoint it needs to be delivered to
                WayPointID myWayPointID;
                myWayPointID = allSubsets.CurrentKey ();
                WayPoint &myWayPoint = myWayPoints.Find (myWayPointID);

                // put the destination query exits into it
                temp2.get_dest ().swap (allSubsets.CurrentData ());
#ifdef DEBUG
                string QEs;
                temp2.get_dest ().MoveToStart();
                while (temp2.get_dest ().RightLength()) {
                    QEs += temp2.get_dest ().Current().GetStr();
                    QEs += " ";
                    temp2.get_dest ().Advance();
                }
#endif
                // and deliver it
                temp2.get_currentPos ().swap (myWayPointID);
                PDEBUG("Sending DATA message to %s with current pos = %s and destination Query Exits = %s",
                        myWayPoint.GetID().getName().c_str(), temp2.get_currentPos ().GetStr().c_str(), QEs.c_str());
                DIAGNOSE_ENTRY("ExecutionEngine", myWayPoint.GetID().getName().c_str(), "DATA");
                myWayPoint.ProcessHoppingDataMsg (temp2);

                // and move on in the list of subsets
                allSubsets.Advance ();
            }

            // and get outta here!
            return 1;

            /*********************/
        } case MessageType::CPU_TOKEN_REQUEST: {

            // take out the CPU request
            TokenRequest &whoIsAsking = requestListCPU.front();
            requestListCPU.pop_front();

            // now we have the CPU request... so we will make sure it has a high enough priority
            if (whoIsAsking.priority > GetPriorityCutoff (CPUWorkToken::type)) {

                // if we got in there, it is not high enough priority, so we just buffer it
                // for future use... if the priority cutoff changes in the future, we will
                // go ahead and try to process all of these requests
                frozenOutFromCPU.emplace_back(move(whoIsAsking));
                return 1;
            }

            // take out the token
            CPUWorkToken &workToken = unusedCPUTokens.front();
            unusedCPUTokens.pop_front();

            if (!myWayPoints.IsThere (whoIsAsking.whoIsAsking)) {
                FATAL ("I could not find a waypoint who had requested a token!");
            }

            WayPoint &thisOne = myWayPoints.Find (whoIsAsking.whoIsAsking);
            thisOne.RequestGranted (workToken);
            return 1;

            /*********************/
        } case MessageType::DISK_TOKEN_REQUEST: {

            // take out the Disk request
            TokenRequest &whoIsAsking = requestListDisk.front();
            requestListDisk.pop_front();

            // now we have the Disk request... so we will make sure it has a high enough priority
            if (whoIsAsking.priority > GetPriorityCutoff (DiskWorkToken::type)) {

                // if we got in there, it is not high enough priority, so we just buffer it
                // for future use... if the priority cutoff changes in the future, we will
                // go ahead and try to process all of these requests
                frozenOutFromDisk.emplace_back(move(whoIsAsking));
                return 1;
            }

            // take out the token
            DiskWorkToken &workToken = unusedDiskTokens.front();
            unusedDiskTokens.pop_front();

            if (!myWayPoints.IsThere (whoIsAsking.whoIsAsking)) {
                FATAL ("I could not find a waypoint who had requested a token!");
            }

            WayPoint &thisOne = myWayPoints.Find (whoIsAsking.whoIsAsking);
            thisOne.RequestGranted (workToken);
            return 1;

            /***********/
        } default:

            FATAL ("Got some weird request into the queue.\n");

    }

}

// If we have higher priority guys waiting, ignore this request
int ExecEngineImp :: RequestTokenImmediate (WayPointID &whoIsAsking, off_t requestType, GenericWorkToken &returnVal, int priority) {

    // at this point, we are not doing anything fancy: grant the request if it is possible to do so
    //
    // first, we look to give out a CPU work token
    if (requestType == CPUWorkToken::type) {

        if (priority > GetPriorityCutoff (CPUWorkToken::type))
            return 0;

        if (unusedCPUTokens.size() > requestListCPU.size()) {
            returnVal.swap(unusedCPUTokens.front());
            unusedCPUTokens.pop_front();
            return 1;
        }
        return 0;
    }

    // now we look to give out a disk work token
    if (requestType == DiskWorkToken::type) {

        if (priority > GetPriorityCutoff (DiskWorkToken::type))
            return 0;

        if (unusedDiskTokens.size() > requestListDisk.size()) {
            returnVal.swap(unusedDiskTokens.front());
            unusedDiskTokens.pop_front();
            return 1;
        }
        return 0;
    }

    FATAL ("You have asked for an unsupported token type!!\n");
}

void ExecEngineImp :: RequestTokenDelayOK (WayPointID &whoIsAsking, off_t requestType, int priority) {

    // we cannot, so shove the request on a queue
    // first, we look to give out a CPU work token
    if (requestType == CPUWorkToken::type) {

        // create and record the work request
        requestListCPU.emplace_back(move(whoIsAsking), priority);

        // schedule some token delivery in the future
        if (unusedCPUTokens.size() >= requestListCPU.size()) {
            InsertRequest(MessageType::CPU_TOKEN_REQUEST);
        }

    } else if (requestType == DiskWorkToken::type) {

        // create and record the work request
        requestListDisk.emplace_back(move(whoIsAsking), priority);

        if (unusedDiskTokens.size() >= requestListDisk.size()) {
            InsertRequest(MessageType::DISK_TOKEN_REQUEST);
        }

    } else {
        FATAL ("Bad request for a work token.\n");
    }
}

void ExecEngineImp :: RequestTokenDelayMillis (WayPointID &whoIsAsking, off_t requestType, uint64_t millis, int priority) {
    // fulfill a request no earlier than "millis"(input argument) milliseconds from now

    // first, we look to give out a CPU work token
    if (requestType == CPUWorkToken::type) {
        // add to the min priority queue rank by the expected time(current time + millis) to be granted
        delayRequestListCPU.emplace(move(whoIsAsking), priority, millis);
    }

    // now we look to give out a disk work token
    if (requestType == DiskWorkToken::type) {
        // add to the min priority queue rank by the expected time(current time + millis) to be granted
        delayRequestListDisk.emplace(move(whoIsAsking), priority, millis);
    }

    FATAL ("You have asked for an unsupported token type!!\n");

}

void ExecEngineImp :: SendHoppingDataMsg( HoppingDataMsg &sendMe ) {
    FATALIF(sendMe.Type() == ABSTRACT_DATA_TYPE, "Message is invalid");
    FATALIF(CHECK_DATA_TYPE(sendMe.get_data(), ExecEngineData), "Payload is invalid");

    // store the message for later processing
    hoppingDataMessages.emplace_back(move(sendMe));
    InsertRequest(MessageType::HOPPING_DATA_MESSAGE);
}

void ExecEngineImp :: SendHoppingDownstreamMsg (HoppingDownstreamMsg &sendMe) {

    FATALIF(sendMe.Type() == ABSTRACT_DATA_TYPE, "Message is invalid");
    FATALIF(sendMe.get_msg().Type() == ABSTRACT_DATA_TYPE, "Payload is invalid");

    // store the message for later processing
    hoppingDownstreamMessages.emplace_back(move(sendMe));
    InsertRequest (MessageType::HOPPING_DOWNSTREAM_MESSAGE);
}

void ExecEngineImp :: SendHoppingUpstreamMsg (HoppingUpstreamMsg &sendMe) {

    FATALIF(sendMe.Type() == ABSTRACT_DATA_TYPE, "Message is invalid");
    FATALIF(sendMe.get_msg().Type() == ABSTRACT_DATA_TYPE, "Payload is invalid");

    // and store the message for later processing
    hoppingUpstreamMessages.emplace_back(move(sendMe));
    InsertRequest (MessageType::HOPPING_UPSTREAM_MESSAGE);
}

void ExecEngineImp :: SendAckMsg (QueryExitContainer &whichOnes, HistoryList &lineage) {

    // this is easy: just load up the ack
    acks.emplace_back(whichOnes, lineage);
    InsertRequest (MessageType::ACK);
}

void ExecEngineImp :: SendDropMsg (QueryExitContainer &whichOnes, HistoryList &lineage) {

    // just as easy: simply load up the drop
    drops.emplace_back(whichOnes, lineage);
    InsertRequest (MessageType::DROP);
}

void ExecEngineImp :: SendDirectMsg (DirectMsg &sendMe) {

    // and store the message for later processing
    directMessages.emplace_back(move(sendMe));
    InsertRequest (MessageType::DIRECT_MESSAGE);
}

MESSAGE_HANDLER_DEFINITION_BEGIN(ExecEngineImp, ConfigureExecEngine, ConfigureExecEngineMessage) {

    // Run tasks first
    FOREACH_TWL(task, msg.tasks) {
        switch (task.Type()) {
            case DeleteRelationTask::type:
                {
                    DeleteRelationTask myTask;
                    myTask.swap(task);
                    globalDiskPool.DeleteContent(myTask.get_relation());
                }
                break;
            default:
                FATAL("Unknown task type %llx", task.Type());
        }
    } END_FOREACH

    // we go thru the list of guys that are being configured...
    msg.configs.MoveToStart ();
    while (msg.configs.RightLength ()) {

        // get the current configuration
        WayPointConfigureData myConfig;
        msg.configs.Remove (myConfig);

        // if we already have this guy, then just give him his new configuration
        if (evProc.myWayPoints.IsThere (myConfig.get_myID ())) {

            WayPoint &thisOne = evProc.myWayPoints.Find (myConfig.get_myID ());
            thisOne.Configure (myConfig);

            // in the other case, we have to create the new waypoint
        } else {

            // get the new waypoint's ID
            WayPointID myID = myConfig.get_myID ();

            // this creates and configures the waypoint
            WayPoint thisOne;
            thisOne.Configure (myConfig);

            // then we add it to the map
            evProc.myWayPoints.Insert (myID, thisOne);
        }
    }

#ifdef DEBUG
    // remember the new graph
    cout << "Graph is:\n\n ";
    msg.newGraph.Print ();
    cout << "\n\n";
#endif
    msg.newGraph.swap (evProc.myGraph);

    // at this point, we are fully configured, so we process any messages that are waiting to be delivered
    while (evProc.DeliverSomeMessage ());

} MESSAGE_HANDLER_DEFINITION_END

extern CPUWorkerPool myCPUWorkers;
extern CPUWorkerPool myDiskWorkers;

void ExecEngineImp :: GiveBackToken (GenericWorkToken &giveBack) {

    if (CHECK_DATA_TYPE(giveBack, CPUWorkToken)) {

        // store the token for later use
        CPUWorkToken temp;
        temp.swap(giveBack);
        unusedCPUTokens.emplace_back(move(temp));

        // if there was someone waiting on the token, then put in the request
        if (unusedCPUTokens.size() <= requestListCPU.size()) {
            InsertRequest(MessageType::CPU_TOKEN_REQUEST);
        }

        // this next bit of code handles a disk token
    } else if (CHECK_DATA_TYPE(giveBack, DiskWorkToken)) {

        // store the token for later use
        DiskWorkToken temp;
        temp.swap(giveBack);
        unusedDiskTokens.emplace_back(move(temp));

        // if there was someone waiting on the token, then put in the request
        if (unusedDiskTokens.size() <= requestListDisk.size()) {
            InsertRequest(MessageType::DISK_TOKEN_REQUEST);
        }

    } else {
        FATAL ("Got back some sort of work token I have never seen.\n");
    }

}


MESSAGE_HANDLER_DEFINITION_BEGIN(ExecEngineImp, HoppingDataMsgReady, HoppingDataMsgMessage) {

    // first, we let the person who produced this data know that we have gotten it back
    if (evProc.myWayPoints.IsThere (msg.message.get_currentPos ())) {

        WayPoint &thisOne = evProc.myWayPoints.Find (msg.message.get_currentPos ());


        // this makes it so that the guy can keep the token if he wants to
        // putting the token here makes it available for "reclaiming" by the
        // waypoint that originally ran it
        evProc.holdMe.swap (msg.token);
        evProc.holdMeIsValid = 1;

        // tell the WP we are done producing, and allow some post processing if needed
        PDEBUG("Calling DoneProducing for %s", thisOne.GetID().getName().c_str());

        thisOne.DoneProducing (msg.message.get_dest (), msg.message.get_lineage(), msg.returnVal, msg.message.get_data ());

        // give back the token if it is not reclaimed
        if (evProc.holdMeIsValid == 1) {
            evProc.holdMeIsValid = 0;
            evProc.GiveBackToken (evProc.holdMe);
        }

    } else {
        FATAL ("Got some data back from a worker, but I have never seen the producing waypoint.\n");
    }

    // next, we take care of the work token that was given back
    WayPointID foo = msg.message.get_currentPos ();

    // if we did not get a generic (invalid) data object back, then add it to the delivery queue
    if (!CHECK_DATA_TYPE (msg.message.get_data (), ExecEngineData)) {
        evProc.hoppingDataMessages.emplace_back(move(msg.message));
        evProc.InsertRequest (MessageType::HOPPING_DATA_MESSAGE);
    }

    // and then process any messages that are waiting to be delivered
    while (evProc.DeliverSomeMessage ());

} MESSAGE_HANDLER_DEFINITION_END


void ExecEngineImp :: InsertRequest (ExecEngineImp :: MessageType requestID) {
    requests.push_back(requestID);
}

ExecEngineImp ::MessageType ExecEngineImp :: RemoveRequest () {
    MessageType &requestID = requests.front();
    requests.pop_front();
    return requestID;
}

int ExecEngineImp :: GetPriorityCutoff (off_t requestType) {

    if (requestType == CPUWorkToken :: type)
        return priorityCPU;

    if (requestType == DiskWorkToken :: type)
        return priorityDisk;

    FATAL ("You asked for the cutoff for a priority I do not understand.");

}

void ExecEngineImp :: SetPriorityCutoff (off_t requestType, int priority) {

    if (requestType == CPUWorkToken :: type) {

        // set the CPU priority cutoff
        priorityCPU = priority;

        // now we look for anyone who was frozen out from the CPU due to a low priority...
        // if they now have a high enough priority, then we ill add them to the queue
        for (auto it = frozenOutFromCPU.begin(); it != frozenOutFromCPU.end(); ++it) {
            if (it->priority <= priority) {
                RequestTokenDelayOK(it->whoIsAsking, CPUWorkToken :: type, it->priority);
                frozenOutFromCPU.erase(it);
            }
        }

    } else if (requestType == DiskWorkToken :: type) {

        // set the Disk priority cutoff
        priorityDisk = priority;

        // now we look for anyone who was frozen out from the Disk due to a low priority...
        // if they now have a high enough priority, then we will add them to the queue
        for (auto it = frozenOutFromDisk.begin(); it != frozenOutFromDisk.end(); ++it) {
            if (it->priority <= priority) {
                RequestTokenDelayOK(it->whoIsAsking, DiskWorkToken :: type, it->priority);
                frozenOutFromDisk.erase(it);
            }
        }
    } else {
        FATAL ("You set the priority for a resource I do not recognize");
    }

    // and then process any messages that are waiting to be delivered... we do this because there
    // might now be some CPU or disk requests that we can process
    while (DeliverSomeMessage ());
}

void ExecEngineImp :: GrantDelayTokens(off_t requestType) {
    // get current time
    auto now = std::chrono::system_clock::now().time_since_epoch();
    uint64_t nowInMillis = std::chrono::duration_cast<std::chrono::milliseconds>(now).count();

    if (requestType == CPUWorkToken :: type) {

        // now we look for CPU tokens requests who was expected to be granted
        while (!delayRequestListCPU.empty() && nowInMillis >= delayRequestListCPU.top().expectedTimeMillis) {
            DelayTokenRequest& delayToken = const_cast<DelayTokenRequest&>(delayRequestListCPU.top());
            RequestTokenDelayOK(delayToken.whoIsAsking, CPUWorkToken :: type, delayToken.priority);
            delayRequestListDisk.pop();
        }
    } else if (requestType == DiskWorkToken :: type) {

        // now we look for disk tokens requests who was expected to be granted
        while (!delayRequestListDisk.empty() && nowInMillis >= delayRequestListDisk.top().expectedTimeMillis) {
            DelayTokenRequest& delayToken = const_cast<DelayTokenRequest&>(delayRequestListDisk.top());
            RequestTokenDelayOK(delayToken.whoIsAsking, DiskWorkToken :: type, delayToken.priority);
            delayRequestListDisk.pop();
        }
    } else {
        FATAL ("You set the priority for a resource I do not recognize");
    }

    // and then process any messages that are waiting to be delivered... we do this because there
    // might now be some CPU or disk requests that we can process
    while (DeliverSomeMessage ());
}

void ExecEngineImp :: ReclaimToken (GenericWorkToken &putResHere) {

    if (holdMeIsValid == 0) {
        FATAL ("Did someone call ReclaimToken from outside of DoneProducing?");
    } else {
        putResHere.swap (holdMe);
        holdMeIsValid = 0;
    }
}

bool ExecEngineImp :: RegisterService( WayPointID& wp, std::string& serviceID ) {
    auto it = services.find(serviceID);
    if( it == services.end() ) {
        services[serviceID] = wp;
        return true;
    } else {
        WARNING("Attempted to register duplicate service %s to waypoint %s (already registered to %s)",
                serviceID.c_str(),
                wp.getName().c_str(),
                it->second.getName().c_str()
                );
        return false;
    }
}

bool ExecEngineImp :: RemoveService( std::string& serviceID ) {
    auto it = services.find(serviceID);
    if( it != services.end() ) {
        services.erase(it);
        return true;
    } else {
        // Error, service not registered!
        WARNING("Attempted to remove unregistered service %s", serviceID.c_str());
        return false;
    }
}

MESSAGE_HANDLER_DEFINITION_BEGIN(ExecEngineImp, ServiceRequestMessage_H, ServiceRequestMessage) {
    ServiceData& data = msg.request;
    auto it = evProc.services.find(data.get_service());
    if( it != evProc.services.end() ) {
        WayPointID wpID = it->second;
        WayPoint wp = evProc.myWayPoints.Find(wpID);
        wp.ProcessServiceRequest(data);
    } else {
        // No such service, send back an error.
        ServiceData errReply = ServiceErrors::MakeError(data, ServiceErrors::NoSuchService, "No such service");

        evProc.SendServiceReply(errReply);
    }
} MESSAGE_HANDLER_DEFINITION_END

MESSAGE_HANDLER_DEFINITION_BEGIN(ExecEngineImp, ServiceControlMessage_H, ServiceControlMessage) {
    ServiceData& data = msg.control;
    std::string service = data.get_service();
    auto it = evProc.services.find(service);
    if( it != evProc.services.end() ) {
        WayPointID wpID = it->second;
        WayPoint wp = evProc.myWayPoints.Find(wpID);
        wp.ProcessServiceControl(data);
    } else {
        // No such service, send back an error.
        ServiceData errReply = ServiceErrors::MakeError(data, ServiceErrors::NoSuchService, "No such service");

        evProc.SendServiceReply(errReply);
    }
} MESSAGE_HANDLER_DEFINITION_END

void ExecEngineImp :: SendServiceReply( ServiceData& reply ) {
    EventProcessor dest = serviceFrontend;
    ServiceReplyMessage::Factory(dest, reply);
}

void ExecEngineImp :: SendServiceInfo( std::string& serviceID, std::string& status, Json::Value& data ) {
    EventProcessor dest = serviceFrontend;
    ServiceInfoMessage::Factory(dest, serviceID, status, data);
}
