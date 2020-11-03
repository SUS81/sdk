/**
 * @file transferslot.cpp
 * @brief Class for active transfer
 *
 * (c) 2013-2014 by Mega Limited, Auckland, New Zealand
 *
 * This file is part of the MEGA SDK - Client Access Engine.
 *
 * Applications using the MEGA API must present a valid application key
 * and comply with the the rules set forth in the Terms of Service.
 *
 * The MEGA SDK is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 *
 * @copyright Simplified (2-clause) BSD License.
 *
 * You should have received a copy of the license along with this
 * program.
 */

#include "mega/transferslot.h"
#include "mega/node.h"
#include "mega/transfer.h"
#include "mega/megaclient.h"
#include "mega/command.h"
#include "mega/base64.h"
#include "mega/megaapp.h"
#include "mega/utils.h"
#include "mega/logging.h"
#include "mega/raid.h"

namespace mega {

TransferSlotFileAccess::TransferSlotFileAccess(std::unique_ptr<FileAccess>&& p, Transfer* t) 
    : transfer(t)
{
    reset(std::move(p));
}

TransferSlotFileAccess::~TransferSlotFileAccess()
{
    reset();
}

void TransferSlotFileAccess::reset(std::unique_ptr<FileAccess>&& p)
{
    fa = std::move(p);

    // transfer has no slot or slot has no fa: timer is enabled
    transfer->bt.enable(!!p);
}


// transfer attempts are considered failed after XFERTIMEOUT deciseconds
// without data flow
const dstime TransferSlot::XFERTIMEOUT = 600;

// max time without progress callbacks
const dstime TransferSlot::PROGRESSTIMEOUT = 10;

// max request size for downloads
#if defined(__ANDROID__) || defined(USE_IOS) || defined(WINDOWS_PHONE)
    const m_off_t TransferSlot::MAX_REQ_SIZE = 2097152; // 2 MB
#elif defined (_WIN32) || defined(HAVE_AIO_RT)
    const m_off_t TransferSlot::MAX_REQ_SIZE = 16777216; // 16 MB
#else
    const m_off_t TransferSlot::MAX_REQ_SIZE = 4194304; // 4 MB
#endif

TransferSlot::TransferSlot(Transfer* ctransfer)
    : fa(ctransfer->client->fsaccess->newfileaccess(), ctransfer)
    , retrybt(ctransfer->client->rng, ctransfer->client->transferSlotsBackoff)
{
    starttime = 0;
    lastprogressreport = 0;
    progressreported = 0;
    speed = meanSpeed = 0;
    progresscontiguous = 0;

    lastdata = Waiter::ds;
    errorcount = 0;
    lasterror = API_OK;

    failure = false;
    retrying = false;
    
    fileattrsmutable = 0;

    connections = 0;
    asyncIO = NULL;
    pendingcmd = NULL;

    transfer = ctransfer;
    transfer->slot = this;
    transfer->state = TRANSFERSTATE_ACTIVE;

    slots_it = transfer->client->tslots.end();

    maxRequestSize = MAX_REQ_SIZE;
#if defined(_WIN32) && !defined(WINDOWS_PHONE)
    MEMORYSTATUSEX statex;
    memset(&statex, 0, sizeof (statex));
    statex.dwLength = sizeof (statex);
    if (GlobalMemoryStatusEx(&statex))
    {
        LOG_debug << "RAM stats. Free physical: " << statex.ullAvailPhys << "   Free virtual: " << statex.ullAvailVirtual;
        if (statex.ullAvailPhys < 1073741824 // 1024 MB
                || statex.ullAvailVirtual < 1073741824)
        {
            if (statex.ullAvailPhys < 536870912 // 512 MB
                    || statex.ullAvailVirtual < 536870912)
            {
                if (statex.ullAvailPhys < 268435456 // 256 MB
                        || statex.ullAvailVirtual < 268435456)
                {
                    maxRequestSize = 2097152; // 2 MB
                }
                else
                {
                    maxRequestSize = 4194304; // 4 MB
                }
            }
            else
            {
                maxRequestSize = 8388608; // 8 MB
            }
        }
        else
        {
            maxRequestSize = 16777216; // 16 MB
        }
    }
    else
    {
        LOG_warn << "Error getting RAM usage info";
    }
#endif
}

bool TransferSlot::createconnectionsonce()
{
    // delay creating these until we know if it's raid or non-raid
    if (!(connections || reqs.size() || asyncIO))
    {
        if (transferbuf.tempUrlVector().empty())
        {
            return false;   // too soon, we don't know raid / non-raid yet
        }

        connections = transferbuf.isRaid() ? RAIDPARTS : (transfer->size > 131072 ? transfer->client->connections[transfer->type] : 1);
        LOG_debug << "Populating transfer slot with " << connections << " connections, max request size of " << maxRequestSize << " bytes";
        reqs.resize(connections);
        asyncIO = new AsyncIOContext*[connections]();
    }
    return true;
}

// delete slot and associated resources, but keep transfer intact (can be
// reused on a new slot)
TransferSlot::~TransferSlot()
{
    if (transfer->type == GET && !transfer->finished
            && transfer->progresscompleted != transfer->size
            && !transfer->asyncopencontext)
    {
        bool cachetransfer = false; // need to save in cache

        if (fa && fa->asyncavailable())
        {
            for (int i = 0; i < connections; i++)
            {
                if (reqs[i] && reqs[i]->status == REQ_ASYNCIO && asyncIO[i])
                {
                    asyncIO[i]->finish();
                    if (!asyncIO[i]->failed)
                    {
                        LOG_verbose << "Async write succeeded";
                        transferbuf.bufferWriteCompleted(i, true);
                        cachetransfer = true;
                    }
                    else
                    {
                        LOG_verbose << "Async write failed";
                        transferbuf.bufferWriteCompleted(i, false);
                    }
                    reqs[i]->status = REQ_READY;
                }
                delete asyncIO[i];
                asyncIO[i] = NULL;
            }

            // Open the file in synchonous mode
            fa.reset(transfer->client->fsaccess->newfileaccess());
            if (!fa->fopen(transfer->localfilename, false, true))
            {
                fa.reset();
            }
        }

        for (int i = 0; i < connections; i++)
        {
            if (HttpReqDL *downloadRequest = static_cast<HttpReqDL*>(reqs[i].get()))
            {
                switch (static_cast<reqstatus_t>(downloadRequest->status))
                {
                    case REQ_INFLIGHT:
                        if (fa && downloadRequest && downloadRequest->status == REQ_INFLIGHT
                            && downloadRequest->contentlength == downloadRequest->size
                            && downloadRequest->bufpos >= SymmCipher::BLOCKSIZE)
                        {
                            HttpReq::http_buf_t* buf = downloadRequest->release_buf();
                            buf->end -= buf->datalen() % RAIDSECTOR;
                            transferbuf.submitBuffer(i, new TransferBufferManager::FilePiece(downloadRequest->dlpos, buf)); // resets size & bufpos of downloadrequest.
                        }
                        break;

                    case REQ_DECRYPTING:
                        LOG_info << "Waiting for block decryption";
                        std::mutex finalizedMutex; 
                        std::unique_lock<std::mutex> guard(finalizedMutex);
                        auto outputPiece = transferbuf.getAsyncOutputBufferPointer(i);
                        outputPiece->finalizedCV.wait(guard, [&](){ return outputPiece->finalized; });
                        downloadRequest->status = REQ_DECRYPTED;
                        break;
                }
            }
        }

        bool anyData = true;
        while (anyData)
        {
            anyData = false;
            for (int i = 0; i < connections; ++i)
            {
                // synchronous writes for all remaining outstanding data (for raid, there can be a sequence of output pieces.  for non-raid, one piece per connection)
                // check each connection first and then all that were not yet on a connection
                auto outputPiece = transferbuf.getAsyncOutputBufferPointer(i);
                if (outputPiece)
                {
                    if (!outputPiece->finalized)
                    {
                        transfer->client->tmptransfercipher.setkey(transfer->transferkey.data());
                        outputPiece->finalize(true, transfer->size, transfer->ctriv, &transfer->client->tmptransfercipher, &transfer->chunkmacs);
                    }
                    anyData = true;
                    if (fa && fa->fwrite(outputPiece->buf.datastart(), static_cast<unsigned>(outputPiece->buf.datalen()), outputPiece->pos))
                    {

                        LOG_verbose << "Sync write succeeded";
                        transferbuf.bufferWriteCompleted(i, true);
                        cachetransfer = true;
                    }
                    else
                    {
                        LOG_err << "Error caching data at: " << outputPiece->pos;
                        transferbuf.bufferWriteCompleted(i, false);  // throws the data away so we can move on to the next one
                    }
                }
            }
        }

        if (cachetransfer)
        {
            transfer->client->transfercacheadd(transfer, nullptr);
            LOG_debug << "Completed: " << transfer->progresscompleted;
        }
    }

    transfer->slot = NULL;

    if (slots_it != transfer->client->tslots.end())
    {
        // advance main loop iterator if deleting next in line
        if (transfer->client->slotit != transfer->client->tslots.end() && *transfer->client->slotit == this)
        {
            transfer->client->slotit++;
        }

        transfer->client->tslots.erase(slots_it);
        transfer->client->performanceStats.transferFinishes += 1;
    }

    if (pendingcmd)
    {
        pendingcmd->cancel();
    }

    if (transfer->asyncopencontext)
    {
        delete transfer->asyncopencontext;
        transfer->asyncopencontext = NULL;
        transfer->client->asyncfopens--;
    }

    while (connections--)
    {
        delete asyncIO[connections];
    }

    delete[] asyncIO;
}

void TransferSlot::toggleport(HttpReqXfer *req)
{
    if (!memcmp(req->posturl.c_str(), "http:", 5))
    {
       size_t portendindex = req->posturl.find("/", 8);
       size_t portstartindex = req->posturl.find(":", 8);

       if (portendindex != string::npos)
       {
           if (portstartindex == string::npos)
           {
               LOG_debug << "Enabling alternative port for chunk";
               req->posturl.insert(portendindex, ":8080");
           }
           else
           {
               LOG_debug << "Disabling alternative port for chunk";
               req->posturl.erase(portstartindex, portendindex - portstartindex);
           }
       }
    }
}

// abort all HTTP connections
void TransferSlot::disconnect()
{
    for (int i = connections; i--;)
    {
        if (reqs[i])
        {
            reqs[i]->disconnect();
        }
    }
}

int64_t TransferSlot::macsmac(chunkmac_map* m)
{
    return m->macsmac(transfer->transfercipher());
}

int64_t TransferSlot::macsmac_gaps(chunkmac_map* m, size_t g1, size_t g2, size_t g3, size_t g4)
{
    return m->macsmac_gaps(transfer->transfercipher(), g1, g2, g3, g4);
}

bool TransferSlot::checkMetaMacWithMissingLateEntries()
{
    // Due to an old bug, some uploads attached a MAC to the node that was missing some MAC entries 
    // (even though the data was uploaded) - this occurred when a ultoken arrived but one other 
    // final upload connection had not completed at the local end (even though it must have 
    // completed at the server end).  So the file's data is still complete in the cloud.
    // Here we check if the MAC is one of those with a missing entry (or a few if the connection had multiple chunks)

    // last 3 connections, up to 32MB (ie chunks) each, up to two completing after the one that delivered the ultoken
    size_t end = transfer->chunkmacs.size();
    size_t finalN = std::min<int>(32 * 3, end);

    // first check for the most likely - a single connection gap (or two but completely consecutive making a single gap)
    for (size_t countBack = 1; countBack <= finalN; ++countBack)
    {
        size_t start1 = end - countBack; 
        for (size_t len1 = 1; len1 <= 64 && start1 + len1 <= end; ++len1)
        {
            if (transfer->metamac == macsmac_gaps(&transfer->chunkmacs, start1, start1 + len1, end, end))
            {
                LOG_warn << "Found mac gaps were at " << start1 << " " << len1 << " from " << end;
                auto correctMac = macsmac(&transfer->chunkmacs);
                transfer->currentmetamac = correctMac;
                transfer->metamac = correctMac;
                updateMacInKey(correctMac);
                return true;
            }
        }
    }

    // now check for two separate pieces missing (much less likely)
    // limit to checking up to 16Mb pieces wtih up to 8Mb between to avoid excessive CPU
    // takes about 1 second on a fairly modest laptop for a 100Mb file (in a release build)
    int caseschecked = 0;
    finalN = std::min<int>(16 * 2 + 8, transfer->chunkmacs.size());
    for (size_t start1 = end - finalN; start1 < end; ++start1)
    {
        for (size_t len1 = 1; len1 <= 16 && start1 + len1 <= end; ++len1)
        {
            for (size_t start2 = start1 + len1 + 1; start2 < transfer->chunkmacs.size(); ++start2)
            {
                for (size_t len2 = 1; len2 <= 16 && start2 + len2 <= end; ++len2)
                {
                    if (transfer->metamac == macsmac_gaps(&transfer->chunkmacs, start1, start1 + len1, start2, start2 + len2))
                    {
                        LOG_warn << "Found mac gaps were at " << start1 << " " << len1 << " " << start2 << " " << len2 << " from " << end;
                        auto correctMac = macsmac(&transfer->chunkmacs);
                        transfer->currentmetamac = correctMac;
                        transfer->metamac = correctMac;
                        updateMacInKey(correctMac);
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

void TransferSlot::updateMacInKey(int64_t correctMac)
{
    // Update the Node's key to be correct (needs some API additions before enabling)
    //for (file_list::iterator it = transfer->files.begin(); it != transfer->files.end(); it++)
    //{
    //    if ((*it)->hprivate && !(*it)->hforeign)
    //    {
    //        if (Node* n = transfer->client->nodebyhandle((*it)->h))
    //        {
    //            if (n->type == FILENODE && n->nodekey().size() == FILENODEKEYLENGTH)
    //            {
    //                auto k1 = (byte*)n->nodekey().data();
    //                auto k2 = k1 + SymmCipher::KEYLENGTH;
    //                if (transfer->metamac == MemAccess::get<int64_t>((const char*)k2 + sizeof(int64_t)))
    //                {
    //                    SymmCipher::xorblock(k2, k1);
    //                    MemAccess::set<int64_t>(k2 + sizeof(int64_t), correctMac);
    //                    SymmCipher::xorblock(k2, k1);

    //                    handle_vector hv;
    //                    hv.push_back(n->nodehandle);
    //                    transfer->client->reqs.add(new CommandNodeKeyUpdate(transfer->client, &hv));
    //                }
    //            }
    //        }
    //    }
    //}
}

bool TransferSlot::checkDownloadTransferFinished(DBTableTransactionCommitter& committer, MegaClient* client)
{
    if (transfer->progresscompleted == transfer->size)
    {
        if (transfer->progresscompleted)
        {
            transfer->currentmetamac = macsmac(&transfer->chunkmacs);
            transfer->hascurrentmetamac = true;
        }

        // verify meta MAC
        if (!transfer->size
            || (transfer->currentmetamac == transfer->metamac)
            || checkMetaMacWithMissingLateEntries())
        {
            client->transfercacheadd(transfer, &committer);
            if (transfer->progresscompleted != progressreported)
            {
                progressreported = transfer->progresscompleted;
                lastdata = Waiter::ds;

                progress();
            }

            transfer->complete(committer);
        }
        else
        {
            client->sendevent(99431, "MAC verification failed", 0);
            transfer->chunkmacs.clear();
            transfer->failed(API_EKEY, committer);
        }
        return true;
    }
    return false;
}

// file transfer state machine
void TransferSlot::doio(MegaClient* client, DBTableTransactionCommitter& committer)
{
    CodeCounter::ScopeTimer pbt(client->performanceStats.transferslotDoio);

    if (!fa || (transfer->size && transfer->progresscompleted == transfer->size)
            || (transfer->type == PUT && transfer->ultoken))
    {
        if (transfer->type == GET || transfer->ultoken)
        {
            if (fa && transfer->type == GET)
            {
                LOG_debug << "Verifying cached download";
                transfer->currentmetamac = macsmac(&transfer->chunkmacs);
                transfer->hascurrentmetamac = true;

                // verify meta MAC
                if (transfer->currentmetamac == transfer->metamac)
                {
                    return transfer->complete(committer);
                }
                else
                {
                    client->sendevent(99432, "MAC verification failed for cached download", 0);

                    transfer->chunkmacs.clear();
                    return transfer->failed(API_EKEY, committer);
                }
            }

            // this is a pending completion, retry every 200 ms by default
            retrybt.backoff(2);
            retrying = true;

            return transfer->complete(committer);
        }
        else
        {
            client->sendevent(99410, "No upload token available", 0);

            return transfer->failed(API_EINTERNAL, committer);
        }
    }

    retrying = false;
    retrybt.reset();  // in case we don't delete the slot, and in case retrybt.next=1
    transfer->state = TRANSFERSTATE_ACTIVE;

    if (!createconnectionsonce())   // don't use connections, reqs, or asyncIO before this point.
    {
        return;
    }

    dstime backoff = 0;
    m_off_t p = 0;

    if (errorcount > 4)
    {
        LOG_warn << "Failed transfer: too many errors";
        return transfer->failed(lasterror, committer);
    }

    for (int i = connections; i--; )
    {
        if (reqs[i])
        {
            unsigned slowestConnection;
            if (transfer->type == GET && reqs[i]->contentlength == reqs[i]->size && transferbuf.detectSlowestRaidConnection(i, slowestConnection))
            {
                LOG_debug << "Connection " << slowestConnection << " is the slowest to reply, using the other 5.";
                reqs[slowestConnection].reset();
                transferbuf.resetPart(slowestConnection);
                i = connections; 
                continue;
            }

            if (reqs[i]->status == REQ_FAILURE && reqs[i]->httpstatus == 200 && transfer->type == GET && transferbuf.isRaid())  // the request started out successfully, hence status==200 in the reply headers
            {
                // check if we got some data and the failure occured partway through the part chunk.  If so, best not to waste it, convert to success case with less data
                HttpReqDL *downloadRequest = static_cast<HttpReqDL*>(reqs[i].get());
                LOG_debug << "Connection " << i << " received " << downloadRequest->bufpos << " before failing, processing data.";
                if (downloadRequest->contentlength == downloadRequest->size && downloadRequest->bufpos >= RAIDSECTOR)
                {
                    downloadRequest->bufpos -= downloadRequest->bufpos % RAIDSECTOR;  // always on a raidline boundary
                    downloadRequest->size = unsigned(downloadRequest->bufpos);
                    transferbuf.transferPos(i) = downloadRequest->bufpos;
                    downloadRequest->status = REQ_SUCCESS;
                }
            }

            switch (static_cast<reqstatus_t>(reqs[i]->status))
            {
                case REQ_INFLIGHT:
                    p += reqs[i]->transferred(client);

                    assert(reqs[i]->lastdata != NEVER);
                    if (transfer->type == GET && transferbuf.isRaid() && 
                        (Waiter::ds - reqs[i]->lastdata) > (XFERTIMEOUT / 2) &&
                        transferbuf.connectionRaidPeersAreAllPaused(i))
                    {
                        // switch to 5 channel raid to avoid the slow/delayed connection. (or if already switched, try a different 5).  If we already tried too many times then let the usual timeout occur
                        if (tryRaidRecoveryFromHttpGetError(i))
                        {
                            LOG_warn << "Connection " << i << " is slow or stalled, trying the other 5 cloudraid connections";
                            reqs[i]->disconnect();
                            reqs[i]->status = REQ_READY;
                        }
                    }

                    if (reqs[i]->lastdata > lastdata)
                    {
                        // prevent overall timeout if all channels are busy with big chunks for a while
                        lastdata = reqs[i]->lastdata;
                    }

                    break;

                case REQ_SUCCESS:
                    if (client->orderdownloadedchunks && transfer->type == GET && !transferbuf.isRaid() && transfer->progresscompleted != static_cast<HttpReqDL*>(reqs[i].get())->dlpos)
                    {
                        // postponing unsorted chunk
                        p += reqs[i]->size;
                        break;
                    }

                    lastdata = Waiter::ds;
                    transfer->lastaccesstime = m_time();

                    if (!transferbuf.isRaid())
                    {
                        LOG_debug << "Transfer request finished (" << transfer->type << ") Position: " << transferbuf.transferPos(i) << " (" << transfer->pos << ") Size: " << reqs[i]->size
                            << " Completed: " << (transfer->progresscompleted + reqs[i]->size) << " of " << transfer->size;
                    }
                    else
                    {
                        LOG_debug << "Transfer request finished (" << transfer->type << ") " << " on connection " << i << " part pos: " << transferbuf.transferPos(i) << " of part size " << transferbuf.raidPartSize(i, transfer->size)
                            << " Overall Completed: " << (transfer->progresscompleted) << " of " << transfer->size;
                    }

                    if (transfer->type == PUT)
                    {
                        // completed put transfers are signalled through the
                        // return of the upload token
                        if (reqs[i]->in.size())
                        {
                            if (reqs[i]->in.size() == NewNode::UPLOADTOKENLEN)
                            {                                        
                                LOG_debug << "Upload token received";
                                if (!transfer->ultoken)
                                {
                                    transfer->ultoken.reset(new byte[NewNode::UPLOADTOKENLEN]());
                                }

                                bool tokenOK = true;
                                if (reqs[i]->in.data()[NewNode::UPLOADTOKENLEN - 1] == 1)
                                {
                                    LOG_debug << "New style upload token";
                                    memcpy(transfer->ultoken.get(), reqs[i]->in.data(), NewNode::UPLOADTOKENLEN);
                                }
                                else
                                {
                                    LOG_debug << "Old style upload token: " << reqs[i]->in;
                                    tokenOK = (Base64::atob(reqs[i]->in.data(), transfer->ultoken.get(), NewNode::UPLOADTOKENLEN)
                                               == NewNode::OLDUPLOADTOKENLEN);
                                }

                                if (tokenOK)
                                {
                                    errorcount = 0;
                                    transfer->failcount = 0;

                                    // any other connections that have not reported back yet, or we haven't processed yet,
                                    // must have completed also - make sure to include their chunk MACs in the mac-of-macs
                                    for (int j = connections; j--; )
                                    {
                                        if (j != i && reqs[j] &&
                                              (reqs[j]->status == REQ_INFLIGHT
                                            || reqs[j]->status == REQ_SUCCESS
                                            || reqs[j]->status == REQ_FAILURE)) // could be a network error getting the result
                                        {
                                            LOG_debug << "Including chunk MACs from incomplete/unprocessed (at this end) connection " << j;
                                            transfer->progresscompleted += reqs[j]->size;
                                            transfer->chunkmacs.finishedUploadChunks(static_cast<HttpReqUL*>(reqs[j].get())->mChunkmacs);
                                        }
                                    }

                                    transfer->chunkmacs.finishedUploadChunks(static_cast<HttpReqUL*>(reqs[i].get())->mChunkmacs);
                                    transfer->progresscompleted += reqs[i]->size;
                                    assert(transfer->progresscompleted == transfer->size);

                                    updatecontiguousprogress();

                                    memcpy(transfer->filekey, transfer->transferkey.data(), sizeof transfer->transferkey);
                                    ((int64_t*)transfer->filekey)[2] = transfer->ctriv;
                                    ((int64_t*)transfer->filekey)[3] = macsmac(&transfer->chunkmacs);
                                    SymmCipher::xorblock(transfer->filekey + SymmCipher::KEYLENGTH, transfer->filekey);

                                    client->transfercacheadd(transfer, &committer);

                                    if (transfer->progresscompleted != progressreported)
                                    {
                                        progressreported = transfer->progresscompleted;
                                        lastdata = Waiter::ds;

                                        progress();
                                    }

                                    return transfer->complete(committer);
                                }
                                else
                                {
                                    transfer->ultoken.reset();
                                }
                            }

                            LOG_debug << "Error uploading chunk: " << reqs[i]->in;
                            error e = (error)atoi(reqs[i]->in.c_str());
                            if (e == API_EKEY)
                            {
                                client->sendevent(99429, "Integrity check failed in upload", 0);

                                lasterror = e;
                                errorcount++;
                                reqs[i]->status = REQ_PREPARED;
                                break;
                            }

                            if (e == DAEMON_EFAILED || (reqs[i]->contenttype.find("text/html") != string::npos
                                    && !memcmp(reqs[i]->posturl.c_str(), "http:", 5)))
                            {
                                client->usehttps = true;
                                client->app->notify_change_to_https();

                                if (e == DAEMON_EFAILED)
                                {
                                    // megad returning -4 should result in restarting the transfer
                                    client->sendevent(99440, "Retry requested by storage server", 0);
                                }
                                else
                                {
                                    LOG_warn << "Invalid Content-Type detected during upload: " << reqs[i]->contenttype;
                                }
                                client->sendevent(99436, "Automatic change to HTTPS", 0);

                                return transfer->failed(API_EAGAIN, committer);
                            }

                            // fail with returned error
                            return transfer->failed(e, committer);
                        }

                        transfer->chunkmacs.finishedUploadChunks(static_cast<HttpReqUL*>(reqs[i].get())->mChunkmacs);
                        transfer->progresscompleted += reqs[i]->size;

                        updatecontiguousprogress();

                        if (transfer->progresscompleted == transfer->size)
                        {
                            client->sendevent(99409, "No upload token received", 0);

                            return transfer->failed(API_EINTERNAL, committer);
                        }

                        errorcount = 0;
                        transfer->failcount = 0;
                        client->transfercacheadd(transfer, &committer);
                        reqs[i]->status = REQ_READY;
                    }
                    else   // GET
                    {
                        HttpReqDL *downloadRequest = static_cast<HttpReqDL*>(reqs[i].get());
                        if (reqs[i]->size == reqs[i]->bufpos || downloadRequest->buffer_released)   // downloadRequest->buffer_released being true indicates we're retrying this asyncIO
                        {

                            if (!downloadRequest->buffer_released)
                            {
                                transferbuf.submitBuffer(i, new TransferBufferManager::FilePiece(downloadRequest->dlpos, downloadRequest->release_buf())); // resets size & bufpos.  finalize() is taken care of in the transferbuf
                                downloadRequest->buffer_released = true;
                            }

                            auto outputPiece = transferbuf.getAsyncOutputBufferPointer(i);
                            if (outputPiece)
                            {
                                bool parallelNeeded = outputPiece->finalize(false, transfer->size, transfer->ctriv, transfer->transfercipher(), &transfer->chunkmacs);

                                if (parallelNeeded)
                                {
                                    // do full chunk (and chunk-remainder) decryption on a thread for throughput and to minimize mutex lock times.
                                    auto req = reqs[i];   // shared_ptr for shutdown safety
                                    auto transferkey = transfer->transferkey;
                                    auto ctriv = transfer->ctriv;
                                    auto filesize = transfer->size;
                                    req->status = REQ_DECRYPTING;

                                    client->mAsyncQueue.push([req, outputPiece, transferkey, ctriv, filesize](SymmCipher& sc)
                                    {
                                        sc.setkey(transferkey.data());
                                        outputPiece->finalize(true, filesize, ctriv, &sc, nullptr);
                                        req->status = REQ_DECRYPTED;
                                    }, false);  // not discardable:  if we downloaded the data, don't waste it - decrypt and write as much as we can to file
                                }
                                else
                                {
                                    reqs[i]->status = REQ_DECRYPTED;
                                }
                            }
                            else if (transferbuf.isRaid())
                            {
                                reqs[i]->status = REQ_READY;  // this connection has retrieved a part of the file, but we don't have enough to combine yet for full file output.   This connection can start fetching the next piece of that part.
                            }
                            else
                            {
                                assert(false);  // non-raid, if the request succeeded then we must have a piece to write to file.
                            }
                        }
                        else
                        {
                            if (reqs[i]->contenttype.find("text/html") != string::npos
                                    && !memcmp(reqs[i]->posturl.c_str(), "http:", 5))
                            {
                                LOG_warn << "Invalid Content-Type detected during download: " << reqs[i]->contenttype;
                                client->usehttps = true;
                                client->app->notify_change_to_https();

                                client->sendevent(99436, "Automatic change to HTTPS", 0);

                                return transfer->failed(API_EAGAIN, committer);
                            }

                            client->sendevent(99430, "Invalid chunk size", 0);

                            LOG_warn << "Invalid chunk size: " << reqs[i]->size << " - " << reqs[i]->bufpos;
                            lasterror = API_EREAD;
                            errorcount++;
                            reqs[i]->status = REQ_PREPARED;
                            break;
                        }
                    }
                    break;

                case REQ_DECRYPTED:
                    {
                        assert(transfer->type == GET);

                        // this must return the same piece we just decrypted, since we have not asked the transferbuf to discard it yet.
                        auto outputPiece = transferbuf.getAsyncOutputBufferPointer(i);
                        
                        if (fa->asyncavailable())
                        {
                            if (asyncIO[i])
                            {
                                LOG_warn << "Retrying failed async write";
                                delete asyncIO[i];
                                asyncIO[i] = NULL;
                            }

                            p += outputPiece->buf.datalen();

                            LOG_debug << "Writing data asynchronously at " << outputPiece->pos << " to " << (outputPiece->pos + outputPiece->buf.datalen());
                            asyncIO[i] = fa->asyncfwrite(outputPiece->buf.datastart(), static_cast<unsigned>(outputPiece->buf.datalen()), outputPiece->pos);
                            reqs[i]->status = REQ_ASYNCIO;
                        }
                        else
                        {
                            if (fa->fwrite(outputPiece->buf.datastart(), static_cast<unsigned>(outputPiece->buf.datalen()), outputPiece->pos))
                            {
                                LOG_verbose << "Sync write succeeded";
                                transferbuf.bufferWriteCompleted(i, true);
                                errorcount = 0;
                                transfer->failcount = 0;
                                updatecontiguousprogress();
                            }
                            else
                            {
                                LOG_err << "Error saving finished chunk";
                                if (!fa->retry)
                                {
                                    transferbuf.bufferWriteCompleted(i, false);  // discard failed data so we don't retry on slot deletion
                                    return transfer->failed(API_EWRITE, committer);
                                }
                                lasterror = API_EWRITE;
                                backoff = 2;
                                break;
                            }

                            if (checkDownloadTransferFinished(committer, client))
                            {
                                return;
                            }

                            client->transfercacheadd(transfer, &committer);
                            reqs[i]->status = REQ_READY;
                        }
                    }
                    break;

                case REQ_ASYNCIO:
                    if (asyncIO[i]->finished)
                    {
                        LOG_verbose << "Processing finished async fs operation";
                        if (!asyncIO[i]->failed)
                        {
                            if (transfer->type == PUT)
                            {
                                LOG_verbose << "Async read succeeded";
                                m_off_t npos = asyncIO[i]->pos + asyncIO[i]->len;
                                string finaltempurl = transferbuf.tempURL(i);
                                if (client->usealtupport && !memcmp(finaltempurl.c_str(), "http:", 5))
                                {
                                    size_t index = finaltempurl.find("/", 8);
                                    if(index != string::npos && finaltempurl.find(":", 8) == string::npos)
                                    {
                                        finaltempurl.insert(index, ":8080");
                                    }
                                }

                                auto pos = asyncIO[i]->pos;
                                auto req = reqs[i];    // shared_ptr so no object is deleted out from under the worker
                                auto transferkey = transfer->transferkey;
                                auto ctriv = transfer->ctriv;
                                req->pos = pos;
                                req->status = REQ_ENCRYPTING;

                                client->mAsyncQueue.push([req, transferkey, ctriv, finaltempurl, pos, npos](SymmCipher& sc)
                                    {
                                        sc.setkey(transferkey.data());
                                        req->prepare(finaltempurl.c_str(), &sc, ctriv, pos, npos); 
                                        req->status = REQ_PREPARED;
                                    }, true);   // discardable - if the transfer or client are being destroyed, we won't be sending that data.
                            }
                            else
                            {
                                LOG_verbose << "Async write succeeded";
                                transferbuf.bufferWriteCompleted(i, true);
                                errorcount = 0;
                                transfer->failcount = 0;

                                updatecontiguousprogress();

                                if (checkDownloadTransferFinished(committer, client))
                                {
                                    return;
                                }

                                client->transfercacheadd(transfer, &committer);
                                reqs[i]->status = REQ_READY;

                                if (client->orderdownloadedchunks && !transferbuf.isRaid())
                                {
                                    // Check connections again looking for postponed chunks
                                    delete asyncIO[i];
                                    asyncIO[i] = NULL;
                                    i = connections;
                                    continue;
                                }
                            }
                            delete asyncIO[i];
                            asyncIO[i] = NULL;
                        }
                        else
                        {
                            LOG_warn << "Async operation failed: " << asyncIO[i]->retry;
                            if (!asyncIO[i]->retry)
                            {
                                transferbuf.bufferWriteCompleted(i, false);  // discard failed data so we don't retry on slot deletion
                                delete asyncIO[i];
                                asyncIO[i] = NULL;
                                return transfer->failed(transfer->type == PUT ? API_EREAD : API_EWRITE, committer);
                            }

                            // retry shortly
                            if (transfer->type == PUT)
                            {
                                lasterror = API_EREAD;
                                reqs[i]->status = REQ_READY;
                            }
                            else
                            {
                                lasterror = API_EWRITE;
                                reqs[i]->status = REQ_SUCCESS;
                            }
                            backoff = 2;
                        }
                    }
                    else if (transfer->type == GET)
                    {
                        p += asyncIO[i]->len;
                    }
                    break;

                case REQ_FAILURE:
                    LOG_warn << "Failed chunk. HTTP status: " << reqs[i]->httpstatus << " on channel " << i;
                    if (reqs[i]->httpstatus && reqs[i]->contenttype.find("text/html") != string::npos
                            && !memcmp(reqs[i]->posturl.c_str(), "http:", 5))
                    {
                        LOG_warn << "Invalid Content-Type detected on failed chunk: " << reqs[i]->contenttype;
                        client->usehttps = true;
                        client->app->notify_change_to_https();

                        client->sendevent(99436, "Automatic change to HTTPS", 0);

                        return transfer->failed(API_EAGAIN, committer);
                    }

                    if (reqs[i]->httpstatus == 509)
                    {
                        if (reqs[i]->timeleft < 0)
                        {
                            client->sendevent(99408, "Overquota without timeleft", 0);
                        }

                        LOG_warn << "Bandwidth overquota from storage server";
                        if (reqs[i]->timeleft > 0)
                        {
                            backoff = dstime(reqs[i]->timeleft * 10);
                        }
                        else
                        {
                            // default retry intervals
                            backoff = MegaClient::DEFAULT_BW_OVERQUOTA_BACKOFF_SECS * 10;
                        }

                        return transfer->failed(API_EOVERQUOTA, committer, backoff);
                    }
                    else if (reqs[i]->httpstatus == 429)  
                    {
                        // too many requests - back off a bit (may be added serverside at some point.  Added here 202020623)
                        backoff = 5;
                        reqs[i]->status = REQ_PREPARED;
                    }
                    else if (reqs[i]->httpstatus == 503 && !transferbuf.isRaid())
                    {
                        // for non-raid, if a file gets a 503 then back off as it may become available shortly
                        backoff = 50;
                        reqs[i]->status = REQ_PREPARED;
                    }
                    else if (reqs[i]->httpstatus == 403 || reqs[i]->httpstatus == 404 || (reqs[i]->httpstatus == 503 && transferbuf.isRaid()))
                    {
                        // - 404 means "malformed or expired URL" - can be immediately fixed by getting a fresh one from the API
                        // - 503 means "the API gave you good information, but I don't have the file" - cannot be fixed (at least not immediately) by getting a fresh URL
                        // for raid parts and 503, it's appropriate to try another raid source
                        if (!tryRaidRecoveryFromHttpGetError(i))
                        {
                            return transfer->failed(API_EAGAIN, committer);
                        }
                    }
                    else if (reqs[i]->httpstatus == 0 && tryRaidRecoveryFromHttpGetError(i))
                    {
                        // status 0 indicates network error or timeout; no headers recevied.
                        // tryRaidRecoveryFromHttpGetError has switched to loading a different part instead of this one.
                    }
                    else
                    {
                        if (!failure)
                        {
                            failure = true;
                            bool changeport = false;

                            if (transfer->type == GET && client->autodownport && !memcmp(transferbuf.tempURL(i).c_str(), "http:", 5))
                            {
                                LOG_debug << "Automatically changing download port";
                                client->usealtdownport = !client->usealtdownport;
                                changeport = true;
                            }
                            else if (transfer->type == PUT && client->autoupport && !memcmp(transferbuf.tempURL(i).c_str(), "http:", 5))
                            {
                                LOG_debug << "Automatically changing upload port";
                                client->usealtupport = !client->usealtupport;
                                changeport = true;
                            }

                            client->app->transfer_failed(transfer, API_EFAILED);
                            client->setchunkfailed(&reqs[i]->posturl);
                            ++client->performanceStats.transferTempErrors;

                            if (changeport)
                            {
                                toggleport(reqs[i].get());
                            }
                        }
                        reqs[i]->status = REQ_PREPARED;
                    }

                default:
                    ;
            }
        }

        if (!failure)
        {
            if (!reqs[i] || (reqs[i]->status == REQ_READY))
            {
                bool newInputBufferSupplied = false;
                bool pauseConnectionInputForRaid = false;
                std::pair<m_off_t, m_off_t> posrange = transferbuf.nextNPosForConnection(i, maxRequestSize, connections, newInputBufferSupplied, pauseConnectionInputForRaid, client->httpio->uploadSpeed);

                // we might have a raid-reassembled block to write, or a previously loaded block, or a skip block to process.
                bool newOutputBufferSupplied = false;
                auto outputPiece = transferbuf.getAsyncOutputBufferPointer(i);
                if (outputPiece && reqs[i])
                {
                    // set up to do the actual write on the next loop, as if it was a retry
                    reqs[i]->status = REQ_SUCCESS;
                    static_cast<HttpReqDL*>(reqs[i].get())->buffer_released = true;
                    newOutputBufferSupplied = true;
                }

                if (newOutputBufferSupplied || newInputBufferSupplied || pauseConnectionInputForRaid)
                {
                    // process supplied block, or just wait until other connections catch up a bit
                }
                else if (posrange.second > posrange.first || !transfer->size || (transfer->type == PUT && asyncIO[i]))
                {
                    // download/upload specified range

                    if (!reqs[i])
                    {
                        reqs[i].reset(transfer->type == PUT ? (HttpReqXfer*)new HttpReqUL() : (HttpReqXfer*)new HttpReqDL());
                        reqs[i]->logname = client->clientname + (transfer->type == PUT ? "U" : "D") + std::to_string(++client->transferHttpCounter) + " ";
                    }

                    bool prepare = true;
                    if (transfer->type == PUT)
                    {
                        m_off_t pos = posrange.first;
                        unsigned size = (unsigned)(posrange.second - pos);

                        // No need to keep recopying already processed macs from prior uploads on this req[i]
                        // For uploads, these are always on chunk boundaries so no need to worry about partials.
                        static_cast<HttpReqUL*>(reqs[i].get())->mChunkmacs.clear();

                        if (fa->asyncavailable())
                        {
                            if (asyncIO[i])
                            {
                                LOG_warn << "Retrying a failed read";
                                pos = asyncIO[i]->pos;
                                size = asyncIO[i]->len;
                                posrange.second = pos + size;
                                delete asyncIO[i];
                                asyncIO[i] = NULL;
                            }

                            asyncIO[i] = fa->asyncfread(reqs[i]->out, size, (-(int)size) & (SymmCipher::BLOCKSIZE - 1), pos);
                            reqs[i]->status = REQ_ASYNCIO;
                            prepare = false;
                        }
                        else
                        {
                            if (!fa->fread(reqs[i]->out, size, (-(int)size) & (SymmCipher::BLOCKSIZE - 1), transfer->pos))
                            {
                                LOG_warn << "Error preparing transfer: " << fa->retry;
                                if (!fa->retry)
                                {
                                    return transfer->failed(API_EREAD, committer);
                                }

                                // retry the read shortly
                                backoff = 2;
                                posrange.second = transfer->pos;
                                prepare = false;
                            }
                        }
                    }

                    if (prepare)
                    {
                        string finaltempurl = transferbuf.tempURL(i);
                        if (transfer->type == GET && client->usealtdownport
                                && !memcmp(finaltempurl.c_str(), "http:", 5))
                        {
                            size_t index = finaltempurl.find("/", 8);
                            if(index != string::npos && finaltempurl.find(":", 8) == string::npos)
                            {
                                finaltempurl.insert(index, ":8080");
                            }
                        }

                        if (transfer->type == PUT && client->usealtupport
                                && !memcmp(finaltempurl.c_str(), "http:", 5))
                        {
                            size_t index = finaltempurl.find("/", 8);
                            if(index != string::npos && finaltempurl.find(":", 8) == string::npos)
                            {
                                finaltempurl.insert(index, ":8080");
                            }
                        }

                        reqs[i]->prepare(finaltempurl.c_str(), transfer->transfercipher(),
                                                               transfer->ctriv,
                                                               posrange.first, posrange.second);
                        reqs[i]->pos = posrange.first;
                        reqs[i]->status = REQ_PREPARED;
                    }

                    transferbuf.transferPos(i) = std::max<m_off_t>(transferbuf.transferPos(i), posrange.second);
                }
                else if (reqs[i])
                {
                    reqs[i]->status = REQ_DONE;

                    if (transfer->type == GET)
                    {
                        // raid reassembly can have several chunks to complete at the end of the file - keep processing till they are all done
                        auto outputPiece = transferbuf.getAsyncOutputBufferPointer(i);
                        if (outputPiece)
                        {
                            // set up to do the actual write on the next loop, as if it was a retry
                            reqs[i]->status = REQ_SUCCESS;
                            static_cast<HttpReqDL*>(reqs[i].get())->buffer_released = true;
                        }
                    }
                }
            }

            if (reqs[i] && (reqs[i]->status == REQ_PREPARED) && !backoff)
            {
                reqs[i]->minspeed = true;
                reqs[i]->post(client);
            }
        }
    }

    if (transfer->type == GET && transferbuf.isRaid())
    {
        // for Raid, additionally we need the raid data that's waiting to be recombined
        p += transferbuf.progress();
    }
    p += transfer->progresscompleted;
    
    if (p != progressreported || (Waiter::ds - lastprogressreport) > PROGRESSTIMEOUT)
    {
        if (p != progressreported)
        {
            m_off_t diff = p - progressreported;
            speed = speedController.calculateSpeed(diff);
            meanSpeed = speedController.getMeanSpeed();
            if (transfer->type == PUT)
            {
                client->httpio->updateuploadspeed(diff);
            }
            else
            {
                client->httpio->updatedownloadspeed(diff);
            }

            progressreported = p;
            lastdata = Waiter::ds;
        }
        lastprogressreport = Waiter::ds;

        progress();
    }

    if (Waiter::ds - lastdata >= XFERTIMEOUT && !failure)
    {
        LOG_warn << "Failed chunk(s) due to a timeout: no data moved for " << (XFERTIMEOUT/10) << " seconds" ;
        failure = true;
        bool changeport = false;

        if (transfer->type == GET && client->autodownport && !memcmp(transferbuf.tempURL(0).c_str(), "http:", 5))
        {
            LOG_debug << "Automatically changing download port due to a timeout";
            client->usealtdownport = !client->usealtdownport;
            changeport = true;
        }
        else if (transfer->type == PUT && client->autoupport && !memcmp(transferbuf.tempURL(0).c_str(), "http:", 5))
        {
            LOG_debug << "Automatically changing upload port due to a timeout";
            client->usealtupport = !client->usealtupport;
            changeport = true;
        }

        bool chunkfailed = false;
        for (int i = connections; i--; )
        {
            if (reqs[i] && reqs[i]->status == REQ_INFLIGHT)
            {
                chunkfailed = true;
                client->setchunkfailed(&reqs[i]->posturl);
                reqs[i]->disconnect();

                if (changeport)
                {
                    toggleport(reqs[i].get());
                }

                reqs[i]->status = REQ_PREPARED;
            }
        }

        if (!chunkfailed)
        {
            LOG_warn << "Transfer failed due to a timeout";
            return transfer->failed(API_EAGAIN, committer);  // either the (this) slot has been deleted, or the whole transfer including slot has been deleted
        }
        else
        {
            LOG_warn << "Chunk failed due to a timeout";
            client->app->transfer_failed(transfer, API_EFAILED);
            ++client->performanceStats.transferTempErrors;
        }
    }

    if (!failure && backoff > 0)
    {
        retrybt.backoff(backoff);
        retrying = true;  // we don't bother checking the `retrybt` before calling `doio` unless `retrying` is set.
    }
}


bool TransferSlot::tryRaidRecoveryFromHttpGetError(unsigned connectionNum)
{
    // If we are downloding a cloudraid file then we may be able to ignore one connection and download from the other 5.
    if (transferbuf.isRaid())
    {
        if (transferbuf.tryRaidHttpGetErrorRecovery(connectionNum))
        {
            // transferbuf is now set up to try a new connection
            reqs[connectionNum]->status = REQ_READY;

            // if the file is nearly complete then some connections might have stopped, but need restarting as they could have skipped portions
            for (unsigned j = connections; j--; )
            {
                if (reqs[j] && reqs[j]->status == REQ_DONE)
                {
                    reqs[j]->status = REQ_READY;
                }
            }
            return true;
        }
        LOG_warn << "Cloudraid transfer failed, too many connection errors";
    }
    return false;
}


// transfer progress notification to app and related files
void TransferSlot::progress()
{
    transfer->client->app->transfer_update(transfer);

    for (file_list::iterator it = transfer->files.begin(); it != transfer->files.end(); it++)
    {
        (*it)->progress();
    }
}

void TransferSlot::updatecontiguousprogress()
{
    chunkmac_map::iterator pcit;
    chunkmac_map &pcchunkmacs = transfer->chunkmacs;
    while ((pcit = pcchunkmacs.find(progresscontiguous)) != pcchunkmacs.end()
           && pcit->second.finished)
    {
        progresscontiguous = ChunkedHash::chunkceil(progresscontiguous, transfer->size);
    }
    if (!transferbuf.tempUrlVector().empty() && transferbuf.isRaid())
    {
        LOG_debug << "Contiguous progress: " << progresscontiguous;
    }
    else
    {
        LOG_debug << "Contiguous progress: " << progresscontiguous << " (" << (transfer->pos - progresscontiguous) << ")";
    }
}

} // namespace
