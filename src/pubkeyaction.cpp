/**
 * @file pubkeyaction.cpp
 * @brief Classes for managing public keys
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

#include "mega/pubkeyaction.h"
#include "mega/megaapp.h"
#include "mega/command.h"

namespace mega {
PubKeyAction::PubKeyAction()
{
    cmd = NULL;
}

PubKeyActionPutNodes::PubKeyActionPutNodes(vector<NewNode>&& newnodes, int ctag, CommandPutNodes::Completion&& c)
    : nn(move(newnodes))
    , completion(move(c))
{
    tag = ctag;
}

void PubKeyActionPutNodes::proc(MegaClient* client, User* u)
{
    if (u && u->pubk.isvalid())
    {
        byte buf[AsymmCipher::MAXKEYLENGTH];
        int t;

        // re-encrypt all node keys to the user's public key
        for (size_t i = nn.size(); i--;)
        {
            if (!(t = u->pubk.encrypt(client->rng, (const byte*)nn[i].nodekey.data(), nn[i].nodekey.size(), buf, sizeof buf)))
            {
                if (completion) completion(API_EINTERNAL, USER_HANDLE, nn, false, tag);
                else client->app->putnodes_result(API_EINTERNAL, USER_HANDLE, nn, false, tag);
                return;
            }

            nn[i].nodekey.assign((char*)buf, t);
        }

        client->reqs.add(new CommandPutNodes(client, NodeHandle(), u->uid.c_str(), NoVersioning, move(nn), tag, PUTNODES_APP, nullptr, move(completion), false));
        // 'canChangeVault' is false here because this code path is to write to user's Inbox, which should not require "vw:1"
    }
    else
    {
        if (completion) completion(API_ENOENT, USER_HANDLE, nn, false, tag);
        else client->app->putnodes_result(API_ENOENT, USER_HANDLE, nn, false, tag);
    }
}

// sharekey distribution request for handle h
PubKeyActionSendShareKey::PubKeyActionSendShareKey(handle h)
{
    sh = h;
}

void PubKeyActionSendShareKey::proc(MegaClient* client, User* u)
{
    Node* n;

    // only the share owner distributes share keys
    if (u && u->pubk.isvalid() && (n = client->nodebyhandle(sh)) && n->sharekey && client->checkaccess(n, OWNER))
    {
        int t;
        byte buf[AsymmCipher::MAXKEYLENGTH];

        if ((t = u->pubk.encrypt(client->rng, n->sharekey->key, SymmCipher::KEYLENGTH, buf, sizeof buf)))
        {
            client->reqs.add(new CommandShareKeyUpdate(client, sh, u->uid.c_str(), buf, t));
        }
    }
}

void PubKeyActionCreateShare::proc(MegaClient* client, User* u)
{
    assert(!client->mKeyManager.isSecure());
    // This class can be removed as soon as isSecure() is true
    // It's the same as the isSecure() code path in MegaClient::setshare
    // but having the public RSA key of the target to send the
    // share key also using the old method

    std::string msg = selfemail;
    Node* n;

    // node vanished: bail
    if (!(n = client->nodebyhandle(h)))
    {
        completion(API_ENOENT, mWritable);
        return;
    }

    if (a == ACCESS_UNKNOWN)
    {
        client->reqs.add(new CommandSetShare(client, n, u, a, 0, NULL, mWritable, msg.c_str(), tag, move(completion)));
        return;
    }

    std::string uid;
    if (u)
    {
        uid = u->uid;
    }

    bool newshare = !n->isShared();

    // if creating a folder link and there's no sharekey already
    if (!n->sharekey && uid.empty())
    {
        assert(newshare);

        byte key[SymmCipher::KEYLENGTH];
        client->rng.genblock(key, sizeof key);
        n->sharekey = new SymmCipher(key);
    }

    if (!n->sharekey)
    {
        LOG_err << "You should first create the key using MegaClient::openShareDialog (setshare)";
        completion(API_EKEY, mWritable);
        return;
    }

    // We need to copy all data because "this" (the object) will be deleted
    // when this function finishes
    handle nodehandle = n->nodehandle;
    std::string shareKey((const char *)n->sharekey->key, SymmCipher::KEYLENGTH);
    bool writable = mWritable;
    accesslevel_t accessLevel = a;
    std::function<void(Error, bool writable)> completionCallback = std::move(completion);
    int reqtag = tag;

    std::function<void()> completeShare =
    [client, uid, nodehandle, accessLevel, newshare, msg, reqtag, shareKey, writable, completionCallback]()
    {
        Node *n = client->nodebyhandle(nodehandle);
        // node vanished: bail
        if (!n)
        {
            completionCallback(API_ENOENT, writable);
            return;
        }

        User *u = client->getUserForSharing(uid.c_str());
        handle userhandle = u ? u->userhandle : UNDEF;
        client->reqs.add(new CommandSetShare(client, n, u, accessLevel, newshare, NULL, writable, msg.c_str(), reqtag,
        [client, uid, userhandle, nodehandle, shareKey, completionCallback](Error e, bool writable)
        {
            if (e || ISUNDEF(userhandle))
            {
                completionCallback(e, writable);
                return;
            }

            std::string encryptedKey = client->mKeyManager.encryptShareKeyTo(userhandle, shareKey);
            if (encryptedKey.empty())
            {
                LOG_debug << "Unable to encrypt share key (contact not verified?). The outshare is pending.";
                completionCallback(e, writable);
                return;
            }

            client->reqs.add(new CommandPendingKeys(client, userhandle, nodehandle, (byte *)encryptedKey.data(),
            [client, uid, nodehandle, writable, completionCallback](Error err)
            {
                if (err)
                {
                    LOG_err << "Error sending share key: " << err;
                    completionCallback(API_OK, writable);
                }
                else
                {
                    LOG_debug << "Share key correctly sent";
                    client->mKeyManager.commit(
                    [client, nodehandle, uid]()
                    {
                        // Changes to apply in the commit
                        client->mKeyManager.removePendingOutShare(nodehandle, uid);
                    },
                    [completionCallback, writable]()
                    {
                        completionCallback(API_OK, writable);
                    });
                }
            }));
        }));
    };

    if (newshare || uid.size())
    {
        client->mKeyManager.commit(
        [client, newshare, nodehandle, shareKey, uid]()
        {
            // Changes to apply in the commit
            if (newshare)
            {
                client->mKeyManager.addOutShareKey(nodehandle, shareKey);
            }

            if (uid.size()) // not a folder link, but a share with a user
            {
                // Add pending outshare;
                client->mKeyManager.addPendingOutShare(nodehandle, uid);
            }
        },
        [completeShare]()
        {
            completeShare();
        });
        return;
    }
    else // folder link on an already shared folder -> no need to update ^!keys
    {
        completeShare();
    }
}

// share node sh with access level sa
PubKeyActionCreateShare::PubKeyActionCreateShare(handle sh, accesslevel_t sa, int ctag, bool writable, const char* personal_representation, std::function<void(Error, bool writeable)> f)
{
    h = sh;
    a = sa;
    tag = ctag;
    mWritable = writable;
    completion = move(f);
    assert(completion);

    if (personal_representation)
    {
        selfemail = personal_representation;
    }
}

void PubKeyActionNotifyApp::proc(MegaClient *client, User *u)
{
    client->app->pubkey_result(u);
}

PubKeyActionNotifyApp::PubKeyActionNotifyApp(int ctag)
{
    tag = ctag;
}

} // namespace
