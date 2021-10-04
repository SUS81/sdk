/**
 * @file node.cpp
 * @brief Classes for accessing local and remote nodes
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

#include "mega/node.h"
#include "mega/megaclient.h"
#include "mega/megaapp.h"
#include "mega/share.h"
#include "mega/serialize64.h"
#include "mega/base64.h"
#include "mega/sync.h"
#include "mega/transfer.h"
#include "mega/transferslot.h"
#include "mega/logging.h"
#include "mega/heartbeats.h"
#include "megafs.h"

namespace mega {

Node::Node(MegaClient* cclient, node_vector* dp, handle h, handle ph,
           nodetype_t t, m_off_t s, handle u, const char* fa, m_time_t ts)
{
    client = cclient;
    outshares = NULL;
    pendingshares = NULL;
    tag = 0;
    appdata = NULL;

    nodehandle = h;
    parenthandle = ph;

    parent = NULL;

    type = t;

    size = s;
    owner = u;

    JSON::copystring(&fileattrstring, fa);

    ctime = ts;

    inshare = NULL;
    sharekey = NULL;
    foreignkey = false;

    plink = NULL;

    memset(&changed, 0, sizeof changed);

    Node* p;

    client->nodes[NodeHandle().set6byte(h)] = this;

    if (t >= ROOTNODE && t <= RUBBISHNODE)
    {
        client->rootnodes[t - ROOTNODE] = h;
    }

    // set parent linkage or queue for delayed parent linkage in case of
    // out-of-order delivery
    if ((p = client->nodebyhandle(ph, true)))
    {
        setparent(p);
    }
    else
    {
        dp->push_back(this);
    }

    client->mFingerprints.newnode(this);
}

Node::~Node()
{
    if (keyApplied())
    {
        client->mAppliedKeyNodeCount--;
        assert(client->mAppliedKeyNodeCount >= 0);
    }

    // abort pending direct reads
    client->preadabort(this);

    // remove node's fingerprint from hash
    if (!client->mOptimizePurgeNodes)
    {
        client->mFingerprints.remove(this);
    }

    if (outshares)
    {
        // delete outshares, including pointers from users for this node
        for (share_map::iterator it = outshares->begin(); it != outshares->end(); it++)
        {
            delete it->second;
        }
        delete outshares;
    }

    if (pendingshares)
    {
        // delete pending shares
        for (share_map::iterator it = pendingshares->begin(); it != pendingshares->end(); it++)
        {
            delete it->second;
        }
        delete pendingshares;
    }


    if (!client->mOptimizePurgeNodes)
    {
        // remove from parent's children
        if (parent)
        {
            parent->children.erase(child_it);
        }

        const Node* fa = firstancestor();
        handle ancestor = fa->nodehandle;
        if (ancestor == client->rootnodes[0] || ancestor == client->rootnodes[1] || ancestor == client->rootnodes[2] || fa->inshare)
        {
            client->mNodeCounters[firstancestor()->nodehandle] -= subnodeCounts();
        }

        if (inshare)
        {
            client->mNodeCounters.erase(nodehandle);
        }

        // delete child-parent associations (normally not used, as nodes are
        // deleted bottom-up)
        for (node_list::iterator it = children.begin(); it != children.end(); it++)
        {
            (*it)->parent = NULL;
        }
    }

    if (plink)
    {
        client->mPublicLinks.erase(nodehandle);
    }

    delete plink;
    delete inshare;
    delete sharekey;
}


Node* Node::childbyname(const string& name)
{
    for (auto* child : children)
    {
        if (child->hasName(name))
            return child;
    }

    return nullptr;
}

bool Node::hasChildWithName(const string& name) const
{
    for (auto* child : children)
    {
        if (child->hasName(name))
            return true;
    }

    return false;
}

/// it's a nice idea but too simple - what about cases where the key is missing or just not available yet (and hence attrs not decrypted yet), etc
//string Node::name() const
//{
//    auto it = attrs.map.find('n');
//
//    if (it != attrs.map.end())
//    {
//        return it->second;
//    }
//
//    return "";
//}

//bool Node::syncable(const LocalNode& parent) const
//{
//    // Not syncable if we're deleted.
//    if (syncdeleted != SYNCDEL_NONE)
//    {
//        return false;
//    }
//
//    // Not syncable if we aren't decrypted.
//    if (attrstring)
//    {
//        return false;
//    }
//
//    auto it = attrs.map.find('n');
//
//    // Not syncable if we don't have a valid name.
//    if (it == attrs.map.end() || it->second.empty())
//    {
//        return false;
//    }
//
//    // We're syncable if we're not the sync debris.
//    return parent.parent || parent.sync->debris != it->second;
//}


void Node::setkeyfromjson(const char* k)
{
    if (keyApplied()) --client->mAppliedKeyNodeCount;
    JSON::copystring(&nodekeydata, k);
    if (keyApplied()) ++client->mAppliedKeyNodeCount;
    assert(client->mAppliedKeyNodeCount >= 0);
}

// update node key and decrypt attributes
void Node::setkey(const byte* newkey)
{
    if (newkey)
    {
        if (keyApplied()) --client->mAppliedKeyNodeCount;
        nodekeydata.assign(reinterpret_cast<const char*>(newkey), (type == FILENODE) ? FILENODEKEYLENGTH : FOLDERNODEKEYLENGTH);
        if (keyApplied()) ++client->mAppliedKeyNodeCount;
        assert(client->mAppliedKeyNodeCount >= 0);
    }

    setattr();
}

// parse serialized node and return Node object - updates nodes hash and parent
// mismatch vector
Node* Node::unserialize(MegaClient* client, const string* d, node_vector* dp)
{
    handle h, ph;
    nodetype_t t;
    m_off_t s;
    handle u;
    const byte* k = NULL;
    const char* fa;
    m_time_t ts;
    const byte* skey;
    const char* ptr = d->data();
    const char* end = ptr + d->size();
    unsigned short ll;
    Node* n;
    int i;
    char isExported = '\0';
    char hasLinkCreationTs = '\0';

    if (ptr + sizeof s + 2 * MegaClient::NODEHANDLE + MegaClient::USERHANDLE + 2 * sizeof ts + sizeof ll > end)
    {
        return NULL;
    }

    s = MemAccess::get<m_off_t>(ptr);
    ptr += sizeof s;

    if (s < 0 && s >= -RUBBISHNODE)
    {
        t = (nodetype_t)-s;
    }
    else
    {
        t = FILENODE;
    }

    h = 0;
    memcpy((char*)&h, ptr, MegaClient::NODEHANDLE);
    ptr += MegaClient::NODEHANDLE;

    ph = 0;
    memcpy((char*)&ph, ptr, MegaClient::NODEHANDLE);
    ptr += MegaClient::NODEHANDLE;

    if (!ph)
    {
        ph = UNDEF;
    }

    u = 0;
    memcpy((char*)&u, ptr, MegaClient::USERHANDLE);
    ptr += MegaClient::USERHANDLE;

    // FIME: use m_time_t / Serialize64 instead
    ptr += sizeof(time_t);

    ts = (uint32_t)MemAccess::get<time_t>(ptr);
    ptr += sizeof(time_t);

    if ((t == FILENODE) || (t == FOLDERNODE))
    {
        int keylen = ((t == FILENODE) ? FILENODEKEYLENGTH : FOLDERNODEKEYLENGTH);

        if (ptr + keylen + 8 + sizeof(short) > end)
        {
            return NULL;
        }

        k = (const byte*)ptr;
        ptr += keylen;
    }

    if (t == FILENODE)
    {
        ll = MemAccess::get<unsigned short>(ptr);
        ptr += sizeof ll;

        if (ptr + ll > end)
        {
            return NULL;
        }

        fa = ptr;
        ptr += ll;
    }
    else
    {
        fa = NULL;
    }

    if (ptr + sizeof isExported + sizeof hasLinkCreationTs > end)
    {
        return NULL;
    }

    isExported = MemAccess::get<char>(ptr);
    ptr += sizeof(isExported);

    hasLinkCreationTs = MemAccess::get<char>(ptr);
    ptr += sizeof(hasLinkCreationTs);

    auto authKeySize = MemAccess::get<char>(ptr);

    ptr += sizeof authKeySize;
    const char *authKey = nullptr;
    if (authKeySize)
    {
        authKey = ptr;
        ptr += authKeySize;
    }

    for (i = 5; i--;)
    {
        if (ptr + (unsigned char)*ptr < end)
        {
            ptr += (unsigned char)*ptr + 1;
        }
    }

    if (ptr + sizeof(short) > end)
    {
        return NULL;
    }

    short numshares = MemAccess::get<short>(ptr);
    ptr += sizeof(numshares);

    if (numshares)
    {
        if (ptr + SymmCipher::KEYLENGTH > end)
        {
            return NULL;
        }

        skey = (const byte*)ptr;
        ptr += SymmCipher::KEYLENGTH;
    }
    else
    {
        skey = NULL;
    }

    n = new Node(client, dp, h, ph, t, s, u, fa, ts);

    if (k)
    {
        n->setkey(k);
    }

    // read inshare, outshares, or pending shares
    while (numshares)   // inshares: -1, outshare/s: num_shares
    {
        int direction = (numshares > 0) ? -1 : 0;
        NewShare *newShare = Share::unserialize(direction, h, skey, &ptr, end);
        if (!newShare)
        {
            LOG_err << "Failed to unserialize Share";
            break;
        }

        client->newshares.push_back(newShare);
        if (numshares > 0)  // outshare/s
        {
            numshares--;
        }
        else    // inshare
        {
            break;
        }
    }

    ptr = n->attrs.unserialize(ptr, end);
    if (!ptr)
    {
        delete n;
        return NULL;
    }

    // It's needed to re-normalize node names because
    // the updated version of utf8proc doesn't provide
    // exactly the same output as the previous one that
    // we were using
    attr_map::iterator it = n->attrs.map.find('n');
    if (it != n->attrs.map.end())
    {
        client->fsaccess->normalize(&(it->second));
    }

    PublicLink *plink = NULL;
    if (isExported)
    {
        if (ptr + MegaClient::NODEHANDLE + sizeof(m_time_t) + sizeof(bool) > end)
        {
            delete n;
            return NULL;
        }

        handle ph = 0;
        memcpy((char*)&ph, ptr, MegaClient::NODEHANDLE);
        ptr += MegaClient::NODEHANDLE;
        m_time_t ets = MemAccess::get<m_time_t>(ptr);
        ptr += sizeof(ets);
        bool takendown = MemAccess::get<bool>(ptr);
        ptr += sizeof(takendown);

        m_time_t cts = 0;
        if (hasLinkCreationTs)
        {
            cts = MemAccess::get<m_time_t>(ptr);
            ptr += sizeof(cts);
        }

        plink = new PublicLink(ph, cts, ets, takendown, authKey ? authKey : "");
        client->mPublicLinks[n->nodehandle] = plink->ph;
    }
    n->plink = plink;

    n->setfingerprint();

    if (ptr == end)
    {
        return n;
    }
    else
    {
        delete n;
        return NULL;
    }
}

// serialize node - nodes with pending or RSA keys are unsupported
bool Node::serialize(string* d)
{
    // do not serialize encrypted nodes
    if (attrstring)
    {
        LOG_warn << "Trying to serialize an encrypted node";

        //Last attempt to decrypt the node
        applykey();
        setattr();

        if (attrstring)
        {
            LOG_warn << "Skipping undecryptable node";
            return false;
        }
    }

    switch (type)
    {
        case FILENODE:
            if ((int)nodekeydata.size() != FILENODEKEYLENGTH)
            {
                return false;
            }
            break;

        case FOLDERNODE:
            if ((int)nodekeydata.size() != FOLDERNODEKEYLENGTH)
            {
                return false;
            }
            break;

        default:
            if (nodekeydata.size())
            {
                return false;
            }
    }

    unsigned short ll;
    short numshares;
    m_off_t s;

    s = type ? -type : size;

    d->append((char*)&s, sizeof s);

    d->append((char*)&nodehandle, MegaClient::NODEHANDLE);

    if (parent)
    {
        d->append((char*)&parent->nodehandle, MegaClient::NODEHANDLE);
    }
    else
    {
        d->append("\0\0\0\0\0", MegaClient::NODEHANDLE);
    }

    d->append((char*)&owner, MegaClient::USERHANDLE);

    // FIXME: use Serialize64
    time_t ts = 0;  // we don't want to break backward compatibiltiy by changing the size (where m_time_t differs)
    d->append((char*)&ts, sizeof(ts));

    ts = (time_t)ctime;
    d->append((char*)&ts, sizeof(ts));

    d->append(nodekeydata);

    if (type == FILENODE)
    {
        ll = static_cast<unsigned short>(fileattrstring.size() + 1);
        d->append((char*)&ll, sizeof ll);
        d->append(fileattrstring.c_str(), ll);
    }

    char isExported = plink ? 1 : 0;
    d->append((char*)&isExported, 1);

    char hasLinkCreationTs = plink ? 1 : 0;
    d->append((char*)&hasLinkCreationTs, 1);

    if (isExported && plink && plink->mAuthKey.size())
    {
        auto authKeySize = (char)plink->mAuthKey.size();
        d->append((char*)&authKeySize, sizeof(authKeySize));
        d->append(plink->mAuthKey.data(), authKeySize);
    }
    else
    {
        d->append("", 1);
    }

    d->append("\0\0\0\0", 5); // Use these bytes for extensions

    if (inshare)
    {
        numshares = -1;
    }
    else
    {
        numshares = 0;
        if (outshares)
        {
            numshares += (short)outshares->size();
        }
        if (pendingshares)
        {
            numshares += (short)pendingshares->size();
        }
    }

    d->append((char*)&numshares, sizeof numshares);

    if (numshares)
    {
        d->append((char*)sharekey->key, SymmCipher::KEYLENGTH);

        if (inshare)
        {
            inshare->serialize(d);
        }
        else
        {
            if (outshares)
            {
                for (share_map::iterator it = outshares->begin(); it != outshares->end(); it++)
                {
                    it->second->serialize(d);
                }
            }
            if (pendingshares)
            {
                for (share_map::iterator it = pendingshares->begin(); it != pendingshares->end(); it++)
                {
                    it->second->serialize(d);
                }
            }
        }
    }

    attrs.serialize(d);

    if (isExported)
    {
        d->append((char*) &plink->ph, MegaClient::NODEHANDLE);
        d->append((char*) &plink->ets, sizeof(plink->ets));
        d->append((char*) &plink->takendown, sizeof(plink->takendown));
        if (hasLinkCreationTs)
        {
            d->append((char*) &plink->cts, sizeof(plink->cts));
        }
    }

    return true;
}

// decrypt attrstring and check magic number prefix
byte* Node::decryptattr(SymmCipher* key, const char* attrstring, size_t attrstrlen)
{
    if (attrstrlen)
    {
        int l = int(attrstrlen * 3 / 4 + 3);
        byte* buf = new byte[l];

        l = Base64::atob(attrstring, buf, l);

        if (!(l & (SymmCipher::BLOCKSIZE - 1)))
        {
            key->cbc_decrypt(buf, l);

            if (!memcmp(buf, "MEGA{\"", 6))
            {
                return buf;
            }
        }

        delete[] buf;
    }

    return NULL;
}

void Node::parseattr(byte *bufattr, AttrMap &attrs, m_off_t size, m_time_t &mtime , string &fileName, string &fingerprint, FileFingerprint &ffp)
{
    JSON json;
    nameid name;
    string *t;

    json.begin((char*)bufattr + 5);
    while ((name = json.getnameid()) != EOO && json.storeobject((t = &attrs.map[name])))
    {
        JSON::unescape(t);
    }

    attr_map::iterator it = attrs.map.find('n');   // filename
    if (it == attrs.map.end())
    {
        fileName = "CRYPTO_ERROR";
    }
    else if (it->second.empty())
    {
        fileName = "BLANK";
    }

    it = attrs.map.find('c');   // checksum
    if (it != attrs.map.end())
    {
        if (ffp.unserializefingerprint(&it->second))
        {
            ffp.size = size;
            mtime = ffp.mtime;

            char bsize[sizeof(size) + 1];
            int l = Serialize64::serialize((byte *)bsize, size);
            char *buf = new char[l * 4 / 3 + 4];
            char ssize = static_cast<char>('A' + Base64::btoa((const byte *)bsize, l, buf));

            string result(1, ssize);
            result.append(buf);
            result.append(it->second);
            delete [] buf;

            fingerprint = result;
        }
    }
}

// return temporary SymmCipher for this nodekey
SymmCipher* Node::nodecipher()
{
    return client->getRecycledTemporaryNodeCipher(&nodekeydata);
}

// decrypt attributes and build attribute hash
void Node::setattr()
{
    byte* buf;
    SymmCipher* cipher;

    if (attrstring && (cipher = nodecipher()) && (buf = decryptattr(cipher, attrstring->c_str(), attrstring->size())))
    {
        JSON json;
        nameid name;
        string* t;

        attrs.map.clear();
        json.begin((char*)buf + 5);

        while ((name = json.getnameid()) != EOO && json.storeobject((t = &attrs.map[name])))
        {
            JSON::unescape(t);

            if (name == 'n')
            {
                client->fsaccess->normalize(t);
            }
        }

        setfingerprint();

        delete[] buf;

        attrstring.reset();
    }
}

// if present, configure FileFingerprint from attributes
// otherwise, the file's fingerprint is derived from the file's mtime/size/key
void Node::setfingerprint()
{
    if (type == FILENODE && nodekeydata.size() >= sizeof crc)
    {
        client->mFingerprints.remove(this);

        attr_map::iterator it = attrs.map.find('c');

        if (it != attrs.map.end())
        {
            if (!unserializefingerprint(&it->second))
            {
                LOG_warn << "Invalid fingerprint";
            }
        }

        // if we lack a valid FileFingerprint for this file, use file's key,
        // size and client timestamp instead
        if (!isvalid)
        {
            memcpy(crc.data(), nodekeydata.data(), sizeof crc);
            mtime = ctime;
        }

        client->mFingerprints.add(this);
    }
}

bool Node::hasName(const string& name) const
{
    auto it = attrs.map.find('n');
    return it != attrs.map.end() && it->second == name;
}

// return file/folder name or special status strings
const char* Node::displayname() const
{
    // not yet decrypted
    if (attrstring)
    {
        LOG_debug << "NO_KEY " << type << " " << size << " " << Base64Str<MegaClient::NODEHANDLE>(nodehandle);
        return "NO_KEY";
    }

    attr_map::const_iterator it;

    it = attrs.map.find('n');

    if (it == attrs.map.end())
    {
        if (type < ROOTNODE || type > RUBBISHNODE)
        {
            LOG_debug << "CRYPTO_ERROR " << type << " " << size << " " << nodehandle;
        }
        return "CRYPTO_ERROR";
    }

    if (!it->second.size())
    {
        LOG_debug << "BLANK " << type << " " << size << " " << nodehandle;
        return "BLANK";
    }

    return it->second.c_str();
}

string Node::displaypath() const
{
    // factored from nearly identical functions in megapi_impl and megacli
    string path;
    const Node* n = this;
    for (; n; n = n->parent)
    {
        switch (n->type)
        {
        case FOLDERNODE:
            path.insert(0, n->displayname());

            if (n->inshare)
            {
                path.insert(0, ":");
                if (n->inshare->user)
                {
                    path.insert(0, n->inshare->user->email);
                }
                else
                {
                    path.insert(0, "UNKNOWN");
                }
                return path;
            }
            break;

        case INCOMINGNODE:
            path.insert(0, "//in");
            return path;

        case ROOTNODE:
            return path.empty() ? "/" : path;

        case RUBBISHNODE:
            path.insert(0, "//bin");
            return path;

        case TYPE_UNKNOWN:
        case FILENODE:
            path.insert(0, n->displayname());
        }
        path.insert(0, "/");
    }
    return path;
}

// returns position of file attribute or 0 if not present
int Node::hasfileattribute(fatype t) const
{
    return Node::hasfileattribute(&fileattrstring, t);
}

int Node::hasfileattribute(const string *fileattrstring, fatype t)
{
    char buf[24];

    sprintf(buf, ":%u*", t);
    return static_cast<int>(fileattrstring->find(buf) + 1);
}

// attempt to apply node key - sets nodekey to a raw key if successful
bool Node::applykey()
{
    if (type > FOLDERNODE)
    {
        //Root nodes contain an empty attrstring
        attrstring.reset();
    }

    if (keyApplied() || !nodekeydata.size())
    {
        return false;
    }

    int l = -1;
    size_t t = 0;
    handle h;
    const char* k = NULL;
    SymmCipher* sc = &client->key;
    handle me = client->loggedin() ? client->me : *client->rootnodes;

    while ((t = nodekeydata.find_first_of(':', t)) != string::npos)
    {
        // compound key: locate suitable subkey (always symmetric)
        h = 0;

        l = Base64::atob(nodekeydata.c_str() + (nodekeydata.find_last_of('/', t) + 1), (byte*)&h, sizeof h);
        t++;

        if (l == MegaClient::USERHANDLE)
        {
            // this is a user handle - reject if it's not me
            if (h != me)
            {
                continue;
            }
        }
        else
        {
            // look for share key if not folder access with folder master key
            if (h != me)
            {
                Node* n;

                // this is a share node handle - check if we have node and the
                // share key
                if (!(n = client->nodebyhandle(h)) || !n->sharekey)
                {
                    continue;
                }

                sc = n->sharekey;

                // this key will be rewritten when the node leaves the outbound share
                foreignkey = true;
            }
        }

        k = nodekeydata.c_str() + t;
        break;
    }

    // no: found => personal key, use directly
    // otherwise, no suitable key available yet - bail (it might arrive soon)
    if (!k)
    {
        if (l < 0)
        {
            k = nodekeydata.c_str();
        }
        else
        {
            return false;
        }
    }

    byte key[FILENODEKEYLENGTH];
    unsigned keylength = (type == FILENODE) ? FILENODEKEYLENGTH : FOLDERNODEKEYLENGTH;

    if (client->decryptkey(k, key, keylength, sc, 0, nodehandle))
    {
        client->mAppliedKeyNodeCount++;
        nodekeydata.assign((const char*)key, keylength);
        setattr();
    }

    assert(keyApplied());
    return true;
}

NodeCounter Node::subnodeCounts() const
{
    NodeCounter nc;
    for (Node *child : children)
    {
        nc += child->subnodeCounts();
    }
    if (type == FILENODE)
    {
        nc.files += 1;
        nc.storage += size;
        if (parent && parent->type == FILENODE)
        {
            nc.versions += 1;
            nc.versionStorage += size;
        }
    }
    else if (type == FOLDERNODE)
    {
        nc.folders += 1;
    }
    return nc;
}

// returns whether node was moved
bool Node::setparent(Node* p)
{
    if (p == parent)
    {
        return false;
    }

    NodeCounter nc;
    bool gotnc = false;

    const Node *originalancestor = firstancestor();
    handle oah = originalancestor->nodehandle;
    if (oah == client->rootnodes[0] || oah == client->rootnodes[1] || oah == client->rootnodes[2] || originalancestor->inshare)
    {
        nc = subnodeCounts();
        gotnc = true;

        // nodes moving from cloud drive to rubbish for example, or between inshares from the same user.
        client->mNodeCounters[oah] -= nc;
    }

    if (parent)
    {
        parent->children.erase(child_it);
    }

    parent = p;

    if (parent)
    {
        //LOG_info << "moving " << Base64Str<MegaClient::NODEHANDLE>(nodehandle) << " " << attrs.map['n'] << " into " << Base64Str<MegaClient::NODEHANDLE>(parent->nodehandle) << " " << parent->attrs.map['n'];
        child_it = parent->children.insert(parent->children.end(), this);
    }

    const Node* newancestor = firstancestor();
    handle nah = newancestor->nodehandle;
    if (nah == client->rootnodes[0] || nah == client->rootnodes[1] || nah == client->rootnodes[2] || newancestor->inshare)
    {
        if (!gotnc)
        {
            nc = subnodeCounts();
        }

        client->mNodeCounters[nah] += nc;
    }

//#ifdef ENABLE_SYNC
//    client->cancelSyncgetsOutsideSync(this);
//#endif

    return true;
}

const Node* Node::firstancestor() const
{
    const Node* n = this;
    while (n->parent != NULL)
    {
        n = n->parent;
    }
    return n;
}

const Node* Node::latestFileVersion() const
{
    const Node* n = this;
    if (type == FILENODE)
    {
        while (n->parent && n->parent->type == FILENODE)
        {
            n = n->parent;
        }
    }
    return n;
}

// returns 1 if n is under p, 0 otherwise
bool Node::isbelow(Node* p) const
{
    const Node* n = this;

    for (;;)
    {
        if (!n)
        {
            return false;
        }

        if (n == p)
        {
            return true;
        }

        n = n->parent;
    }
}

bool Node::isbelow(NodeHandle p) const
{
    const Node* n = this;

    for (;;)
    {
        if (!n)
        {
            return false;
        }

        if (n->nodeHandle() == p)
        {
            return true;
        }

        n = n->parent;
    }
}

void Node::setpubliclink(handle ph, m_time_t cts, m_time_t ets, bool takendown, const string &authKey)
{
    if (!plink) // creation
    {
        assert(client->mPublicLinks.find(nodehandle) == client->mPublicLinks.end());
        plink = new PublicLink(ph, cts, ets, takendown, authKey.empty() ? nullptr : authKey.c_str());
    }
    else            // update
    {
        assert(client->mPublicLinks.find(nodehandle) != client->mPublicLinks.end());
        plink->ph = ph;
        plink->cts = cts;
        plink->ets = ets;
        plink->takendown = takendown;
        plink->mAuthKey = authKey;
    }
    client->mPublicLinks[nodehandle] = ph;
}

PublicLink::PublicLink(handle ph, m_time_t cts, m_time_t ets, bool takendown, const char *authKey)
{
    this->ph = ph;
    this->cts = cts;
    this->ets = ets;
    this->takendown = takendown;
    if (authKey)
    {
        this->mAuthKey = authKey;
    }
}

PublicLink::PublicLink(PublicLink *plink)
{
    this->ph = plink->ph;
    this->cts = plink->cts;
    this->ets = plink->ets;
    this->takendown = plink->takendown;
    this->mAuthKey = plink->mAuthKey;
}

bool PublicLink::isExpired()
{
    if (!ets)       // permanent link: ets=0
        return false;

    m_time_t t = m_time();
    return ets < t;
}

#ifdef ENABLE_SYNC
// set, change or remove LocalNode's parent and localname/slocalname.
// newlocalpath must be a leaf name and must not point to an empty string (unless newparent == NULL).
// no shortname allowed as the last path component.
void LocalNode::setnameparent(LocalNode* newparent, const LocalPath& newlocalpath, std::unique_ptr<LocalPath> newshortname)
{
    Sync* oldsync = NULL;

    if (newshortname && *newshortname == newlocalpath)
    {
        // if the short name is the same, don't bother storing it.
        newshortname.reset();
    }

    bool parentChange = newparent != parent;
    bool localnameChange = newlocalpath != localname;
    bool shortnameChange = newshortname && !slocalname ||
                           slocalname && !newshortname ||
                           newshortname && slocalname && *newshortname != *slocalname;

    if (parent)
    {
        if (parentChange || localnameChange)
        {
            // remove existing child linkage for localname
            parent->children.erase(&localname);
        }

        if (slocalname && (
            parentChange || shortnameChange))
        {
            // remove existing child linkage for slocalname
            parent->schildren.erase(slocalname.get());
            slocalname.reset();
        }
    }

    if (localnameChange)
    {
        // set new name
        localname = newlocalpath;
    }

    if (shortnameChange)
    {
        // set new shortname
        slocalname = move(newshortname);
    }

    // reset treestate for old subtree (in case of just not syncing that subtree anymore - updates icon overlays)
    if (parent && !newparent && !sync->mDestructorRunning)
    {
        // since we can't do it after the parent is updated
        treestate(TREESTATE_NONE);
    }

    if (parentChange)
    {
        parent = newparent;

        if (parent && sync != parent->sync)
        {
            oldsync = sync;
            LOG_debug << "Moving files between different syncs";
        }
    }

    // add to parent map by localname
    if (parent && (parentChange || localnameChange))
    {
        #ifdef DEBUG
            auto it = parent->children.find(&localname);
            assert(it == parent->children.end());   // check we are not about to orphan the old one at this location... if we do then how did we get a clash in the first place?
        #endif

        parent->children[&localname] = this;
    }

    // add to parent map by shortname
    if (parent && slocalname && (parentChange || shortnameChange))
    {
#ifdef DEBUG
// TODO: enable these checks after we're sure about the localname check above
//        auto it = parent->schildren.find(&*slocalname);
//        assert(it == parent->schildren.end());   // check we are not about to orphan the old one at this location... if we do then how did we get a clash in the first place?
#endif
        parent->schildren[&*slocalname] = this;
    }

    // reset treestate
    if (parent && parentChange && !sync->mDestructorRunning)
    {
        treestate(TREESTATE_NONE);
    }

    if (oldsync)
    {
        // prepare localnodes for a sync change or/and a copy operation
        LocalTreeProcMove tp(parent->sync);
        sync->syncs.proclocaltree(this, &tp);

        // add to new parent map by localname// update local cache if there is a sync change
        oldsync->cachenodes();
        sync->cachenodes();
    }

    if (parent && parentChange)
    {
        LocalTreeProcUpdateTransfers tput;
        sync->syncs.proclocaltree(this, &tput);
    }
}

void LocalNode::moveContentTo(LocalNode* ln, LocalPath& fullPath, bool setScanAgain)
{
    vector<LocalNode*> workingList;
    workingList.reserve(children.size());
    for (auto& c : children) workingList.push_back(c.second);
    for (auto& c : workingList)
    {
        ScopedLengthRestore restoreLen(fullPath);
        fullPath.appendWithSeparator(c->localname, true);
        c->setnameparent(ln, fullPath.leafName(), sync->syncs.fsaccess->fsShortname(fullPath));

        // if moving between syncs, removal from old sync db is already done
        ln->sync->statecacheadd(c);

        if (setScanAgain)
        {
            c->setScanAgain(false, true, true, 0);
        }
    }

    ln->transferSP = move(transferSP);

    LocalTreeProcUpdateTransfers tput;
    tput.proc(*sync->syncs.fsaccess, ln);

    ln->mFilterChain = mFilterChain;
    ln->mLoadPending = mLoadPending;

    // Make sure our exclusion state is recomputed.
    ln->setRecomputeExclusionState();
}

// delay uploads by 1.1 s to prevent server flooding while a file is still being written
void LocalNode::bumpnagleds()
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    nagleds = Waiter::ds + 11;
}

LocalNode::LocalNode()
: unstableFsidAssigned(false)
, deletedFS{false}
, moveAppliedToLocal(false)
, moveApplyingToLocal(false)
, conflicts(TREE_RESOLVED)
, scanAgain(TREE_RESOLVED)
, checkMovesAgain(TREE_RESOLVED)
, syncAgain(TREE_RESOLVED)
, parentSetCheckMovesAgain(false)
, parentSetSyncAgain(false)
, parentSetScanAgain(false)
, parentSetContainsConflicts(false)
, fsidSyncedReused(false)
, fsidScannedReused(false)
, scanInProgress(false)
, scanObsolete(false)
, scanBlocked(TREE_RESOLVED)
{}

// initialize fresh LocalNode object - must be called exactly once
void LocalNode::init(Sync* csync, nodetype_t ctype, LocalNode* cparent, const LocalPath& cfullpath, std::unique_ptr<LocalPath> shortname)
{
    sync = csync;
    parent = NULL;
//    notseen = 0;
    unstableFsidAssigned = false;
    deletedFS = false;
    moveAppliedToLocal = false;
    moveApplyingToLocal = false;
    conflicts = TREE_RESOLVED;
    scanAgain = TREE_RESOLVED;
    checkMovesAgain = TREE_RESOLVED;
    syncAgain = TREE_RESOLVED;
    parentSetCheckMovesAgain = false;
    parentSetSyncAgain = false;
    parentSetScanAgain = false;
    parentSetContainsConflicts = false;
    fsidSyncedReused = false;
    fsidScannedReused = false;
    scanInProgress = false;
    scanObsolete = false;
    scanBlocked = TREE_RESOLVED;
    parent_dbid = 0;
    slocalname = NULL;

    ts = TREESTATE_NONE;
    dts = TREESTATE_NONE;

    type = ctype;

    bumpnagleds();

    mLoadPending = false;

    if (cparent)
    {
        setnameparent(cparent, cfullpath.leafName(), std::move(shortname));

        mIsIgnoreFile = type == FILENODE && localname == IGNORE_FILE_NAME;

        auto excluded = parent->isExcluded(localname, type);

        mExcluded = excluded < 0;
        mRecomputeExclusionState = excluded == 0;
    }
    else
    {
        localname = cfullpath;
        slocalname.reset(shortname && *shortname != localname ? shortname.release() : nullptr);

        mExcluded = false;
        mRecomputeExclusionState = false;
    }


//#ifdef DEBUG
//    // double check we were given the right shortname (if the file exists yet)
//    auto fa = sync->client->fsaccess->newfileaccess(false);
//    if (fa->fopen(cfullpath))  // exists, is file
//    {
//        auto sn = sync->client->fsaccess->fsShortname(cfullpath);
//        assert(!localname.empty() &&
//            ((!slocalname && (!sn || localname == *sn)) ||
//                (slocalname && sn && !slocalname->empty() && *slocalname != localname && *slocalname == *sn)));
//    }
//#endif

    // mark fsid as not valid
    fsid_lastSynced_it = sync->syncs.localnodeBySyncedFsid.end();
    fsid_asScanned_it = sync->syncs.localnodeByScannedFsid.end();
    syncedCloudNodeHandle_it = sync->syncs.localnodeByNodeHandle.end();

    sync->syncs.totalLocalNodes++;

    if (type != TYPE_UNKNOWN)
    {
        sync->localnodes[type]++;
    }
}

auto LocalNode::rare() -> RareFields&
{
    if (!rareFields)
    {
        rareFields.reset(new RareFields);
    }
    return *rareFields;
}

auto LocalNode::rareRO() -> const RareFields&
{
    if (!rareFields)
    {
        static RareFields blankFields;
        return blankFields;
    }
    return *rareFields;
}

void LocalNode::trimRareFields()
{
    if (rareFields)
    {
        if (scanBlocked < TREE_ACTION_HERE) rareFields->scanBlockedTimer.reset();
        if (!scanInProgress) rareFields->scanRequest.reset();

        if (!rareFields->scanBlockedTimer &&
            !rareFields->scanRequest &&
            !rareFields->moveFromHere &&
            !rareFields->moveToHere &&
            rareFields->createFolderHere.expired() &&
            rareFields->removeNodeHere.expired() &&
            rareFields->unlinkHere.expired())
        {
            rareFields.reset();
        }
    }
}

unique_ptr<LocalPath> LocalNode::cloneShortname() const
{
    return unique_ptr<LocalPath>(
        slocalname
        ? new LocalPath(*slocalname)
        : nullptr);
}


void LocalNode::setScanAgain(bool doParent, bool doHere, bool doBelow, dstime delayds)
{
    if (doHere && scanInProgress)
    {
        scanObsolete = true;
    }

    unsigned state = (doHere?1u:0u) << 1 | (doBelow?1u:0u);
    if (state >= TREE_ACTION_HERE && delayds > 0)
    {
        if (scanDelayUntil > Waiter::ds + delayds + 10)
        {
            scanDelayUntil = std::max<dstime>(scanDelayUntil,  Waiter::ds + delayds);
        }
        else
        {
            scanDelayUntil = std::max<dstime>(scanDelayUntil,  Waiter::ds + delayds);
        }
    }

    scanAgain = std::max<unsigned>(scanAgain, state);
    for (auto p = parent; p != NULL; p = p->parent)
    {
        p->scanAgain = std::max<unsigned>(p->scanAgain, TREE_DESCENDANT_FLAGGED);
    }

    // for scanning, we only need to set the parent once
    if (parent && doParent)
    {
        parent->scanAgain = std::max<unsigned>(parent->scanAgain, TREE_ACTION_HERE);
        doParent = false;
        parentSetScanAgain = false;
    }
    parentSetScanAgain = parentSetScanAgain || doParent;
}

void LocalNode::setCheckMovesAgain(bool doParent, bool doHere, bool doBelow)
{
    unsigned state = (doHere?1u:0u) << 1 | (doBelow?1u:0u);

    checkMovesAgain = std::max<unsigned>(checkMovesAgain, state);
    for (auto p = parent; p != NULL; p = p->parent)
    {
        p->checkMovesAgain = std::max<unsigned>(p->checkMovesAgain, TREE_DESCENDANT_FLAGGED);
    }

    parentSetCheckMovesAgain = parentSetCheckMovesAgain || doParent;
}

void LocalNode::setSyncAgain(bool doParent, bool doHere, bool doBelow)
{
    unsigned state = (doHere?1u:0u) << 1 | (doBelow?1u:0u);

    syncAgain = std::max<unsigned>(syncAgain, state);
    for (auto p = parent; p != NULL; p = p->parent)
    {
        p->syncAgain = std::max<unsigned>(p->syncAgain, TREE_DESCENDANT_FLAGGED);
    }

    parentSetSyncAgain = parentSetSyncAgain || doParent;
}

void LocalNode::setContainsConflicts(bool doParent, bool doHere, bool doBelow)
{
    // using the 3 flags for consistency & understandabilty but doBelow is not relevant
    assert(!doBelow);

    unsigned state = (doHere?1u:0u) << 1 | (doBelow?1u:0u);

    conflicts = std::max<unsigned>(conflicts, state);
    for (auto p = parent; p != NULL; p = p->parent)
    {
        p->conflicts = std::max<unsigned>(p->conflicts, TREE_DESCENDANT_FLAGGED);
    }

    parentSetContainsConflicts = parentSetContainsConflicts || doParent;
}

void LocalNode::setScanBlocked()
{
    scanBlocked = std::max<unsigned>(scanBlocked, TREE_ACTION_HERE);

    if (!rare().scanBlockedTimer)
    {
        rare().scanBlockedTimer.reset(new BackoffTimer(sync->syncs.rng));
    }
    if (rare().scanBlockedTimer->armed())
    {
        rare().scanBlockedTimer->backoff(Sync::SCANNING_DELAY_DS);
    }

    for (auto p = parent; p != NULL; p = p->parent)
    {
        p->scanBlocked = std::max<unsigned>(p->scanBlocked, TREE_DESCENDANT_FLAGGED);
    }
}

bool LocalNode::checkForScanBlocked(FSNode* fsNode)
{
    if (scanBlocked >= TREE_ACTION_HERE)
    {
        // Have we recovered?
        if (fsNode && fsNode->type != TYPE_UNKNOWN && !fsNode->isBlocked)
        {
            LOG_verbose << sync->syncname << "Recovered from being scan blocked: " << localnodedisplaypath(*sync->syncs.fsaccess);

            type = fsNode->type; // original scan may not have been able to discern type, fix it now
            setScannedFsid(UNDEF, sync->syncs.localnodeByScannedFsid, fsNode->localname);
            sync->statecacheadd(this);

            scanBlocked = TREE_RESOLVED;
            rare().scanBlockedTimer.reset();
            return false;
        }

        // rescan if the timer is up
        if (rare().scanBlockedTimer->armed())
        {
            LOG_verbose << sync->syncname << "Scan blocked timer elapsed, trigger parent rescan: "  << localnodedisplaypath(*sync->syncs.fsaccess);;
            if (parent) parent->setScanAgain(false, true, false, 0);
            rare().scanBlockedTimer->backoff(); // wait increases exponentially
        }
        else
        {
            LOG_verbose << sync->syncname << "Waiting on scan blocked timer, retry in ds: "
                << rare().scanBlockedTimer->retryin() << " for " << getLocalPath().toPath();
        }
        return true;
    }

    if (fsNode && (fsNode->type == TYPE_UNKNOWN || fsNode->isBlocked))
    {
        // We were not able to get details of the filesystem item when scanning the directory.
        // Consider it a blocked file, and we'll rescan the folder from time to time.
        LOG_verbose << sync->syncname << "File/folder was blocked when reading directory, retry later: " << localnodedisplaypath(*sync->syncs.fsaccess);
        setScanBlocked();
        return true;
    }

    return false;
}


bool LocalNode::scanRequired() const
{
    return scanAgain != TREE_RESOLVED;
}

void LocalNode::clearRegeneratableFolderScan(SyncPath& fullPath, vector<syncRow>& childRows)
{
    if (lastFolderScan &&
        lastFolderScan->size() == children.size())
    {
        // check for scan-blocked entries, those are not regeneratable
        for (auto& c : *lastFolderScan)
        {
            if (c.type == TYPE_UNKNOWN) return;
            if (c.isBlocked) return;
        }

        // check that generating the fsNodes results in the same set
        unsigned nChecked = 0;
        for (auto& row : childRows)
        {
            if (!!row.syncNode != !!row.fsNode) return;
            if (row.syncNode && row.fsNode)
            {
                ++nChecked;
                auto generated = row.syncNode->getScannedFSDetails();
                if (!generated.equivalentTo(*row.fsNode)) return;
            }
        }

        if (nChecked == children.size())
        {
            // LocalNodes are now consistent with the last scan.
            LOG_debug << sync->syncname << "Clearing regeneratable folder scan records (" << lastFolderScan->size() << ") at " << fullPath.localPath_utf8();
            lastFolderScan.reset();
        }
    }
}

bool LocalNode::mightHaveMoves() const
{
    return checkMovesAgain != TREE_RESOLVED;
}

bool LocalNode::syncRequired() const
{
    return syncAgain != TREE_RESOLVED;
}


void LocalNode::propagateAnySubtreeFlags()
{
    for (auto& child : children)
    {
        if (child.second->type != FILENODE)
        {
            if (scanAgain == TREE_ACTION_SUBTREE)
            {
                child.second->scanDelayUntil = std::max<dstime>(child.second->scanDelayUntil,  scanDelayUntil);
            }

            child.second->scanAgain = propagateSubtreeFlag(scanAgain, child.second->scanAgain);
            child.second->checkMovesAgain = propagateSubtreeFlag(checkMovesAgain, child.second->checkMovesAgain);
            child.second->syncAgain = propagateSubtreeFlag(syncAgain, child.second->syncAgain);
        }
    }
    if (scanAgain == TREE_ACTION_SUBTREE) scanAgain = TREE_ACTION_HERE;
    if (checkMovesAgain == TREE_ACTION_SUBTREE) checkMovesAgain = TREE_ACTION_HERE;
    if (syncAgain == TREE_ACTION_SUBTREE) syncAgain = TREE_ACTION_HERE;
}


bool LocalNode::processBackgroundFolderScan(syncRow& row, SyncPath& fullPath)
{
    bool syncHere = false;

    assert(row.syncNode == this);
    assert(row.fsNode);
    assert(!sync->localdebris.isContainingPathOf(fullPath.localPath));

    std::shared_ptr<ScanService::ScanRequest> ourScanRequest = scanInProgress ? rare().scanRequest  : nullptr;

    if (!ourScanRequest && (!sync->mActiveScanRequest || sync->mActiveScanRequest->completed()))
    {
        // we can start a single new request if we are still recursing and the last request from this sync completed already
        if (scanDelayUntil != 0 && Waiter::ds < scanDelayUntil)
        {
            LOG_verbose << sync->syncname << "Too soon to scan this folder, needs more ds: " << scanDelayUntil - Waiter::ds;
        }
        else
        {
            // queueScan() already logs: LOG_verbose << "Requesting scan for: " << fullPath.toPath(*client->fsaccess);
            scanObsolete = false;
            scanInProgress = true;

            // If enough details of the scan are the same, we can reuse fingerprints instead of recalculating
            map<LocalPath, FSNode> priorScanChildren;
            for (auto& c : children)
            {
                if (c.second->fsid_lastSynced != UNDEF)
                {
                    assert(*c.first == c.second->localname);
                    priorScanChildren.emplace(*c.first,
                        c.second->scannedFingerprint.isvalid ?
                            c.second->getScannedFSDetails() :
                            c.second->getLastSyncedFSDetails());
                }
            }

            ourScanRequest = sync->syncs.mScanService->queueScan(fullPath.localPath, row.fsNode->fsid, sync->syncs.mClient.followsymlinks, move(priorScanChildren));
            rare().scanRequest = ourScanRequest;
            sync->mActiveScanRequest = ourScanRequest;
        }
    }
    else if (ourScanRequest &&
             ourScanRequest->completed())
    {
        if (ourScanRequest == sync->mActiveScanRequest) sync->mActiveScanRequest.reset();

        scanInProgress = false;

        if (ScanService::SCAN_FSID_MISMATCH == ourScanRequest->completionResult())
        {
            LOG_verbose << sync->syncname << "Directory scan detected outdated fsid : " << fullPath.localPath_utf8();
            scanObsolete = true;
        }

        if (ScanService::SCAN_SUCCESS == ourScanRequest->completionResult()
            && ourScanRequest->fsidScanned() != row.fsNode->fsid)
        {
            LOG_verbose << sync->syncname << "Directory scan returned was for now outdated fsid : " << fullPath.localPath_utf8();
            scanObsolete = true;
        }

        if (scanObsolete)
        {
            LOG_verbose << sync->syncname << "Directory scan outdated for : " << fullPath.localPath_utf8();
            scanObsolete = false;
            scanDelayUntil = Waiter::ds + 10; // don't scan too frequently
        }
        else if (ScanService::SCAN_SUCCESS == ourScanRequest->completionResult())
        {
            lastFolderScan.reset(
                new vector<FSNode>(ourScanRequest->resultNodes()));

            LOG_verbose << sync->syncname << "Received " << lastFolderScan->size() << " directory scan results for: " << fullPath.localPath_utf8();

            scanDelayUntil = Waiter::ds + 20; // don't scan too frequently
            scanAgain = TREE_RESOLVED;
            setSyncAgain(false, true, false);
            syncHere = true;
        }
        else // SCAN_INACCESSIBLE
        {
            // we were previously able to scan this node, but now we can't.
            row.fsNode->isBlocked = true;
            if (!checkForScanBlocked(row.fsNode))
            {
                // start a timer to retry, since we haven't already
                LOG_verbose << sync->syncname << "Directory scan has become inaccesible for path: " << fullPath.localPath_utf8();
                setScanBlocked();
            }
        }
    }

    trimRareFields();
    return syncHere;
}

void LocalNode::reassignUnstableFsidsOnceOnly(const FSNode* fsnode)
{
    if (!sync->fsstableids && !unstableFsidAssigned)
    {
        // for FAT and other filesystems where we can't rely on fsid
        // being the same after remount, so update our previously synced nodes
        // with the actual fsids now attached to them (usually generated by FUSE driver)

        if (fsnode && sync->syncEqual(*fsnode, *this))
        {
            setSyncedFsid(fsnode->fsid, sync->syncs.localnodeBySyncedFsid, localname, fsnode->cloneShortname());
            sync->statecacheadd(this);
        }
        else if (fsid_lastSynced != UNDEF)
        {
            // this node was synced with something, but not the thing that's there now (or not there)
            setSyncedFsid(UNDEF-1, sync->syncs.localnodeBySyncedFsid, localname, fsnode->cloneShortname());
            sync->statecacheadd(this);
        }
        unstableFsidAssigned = true;
    }
}

// update treestates back to the root LocalNode, inform app about changes
void LocalNode::treestate(treestate_t newts)
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    if (newts != TREESTATE_NONE)
    {
        ts = newts;
    }

    if (ts != dts)
    {
		assert(sync->syncs.onSyncThread());
        sync->syncs.mClient.app->syncupdate_treestate(sync->getConfig(), getLocalPath(), ts, type);
    }

    if (parent && ((newts == TREESTATE_NONE && ts != TREESTATE_NONE)
                   || (ts != dts && (!(ts == TREESTATE_SYNCED && parent->ts == TREESTATE_SYNCED))
                                 && (!(ts == TREESTATE_SYNCING && parent->ts == TREESTATE_SYNCING))
                                 && (!(ts == TREESTATE_PENDING && (parent->ts == TREESTATE_PENDING
                                                                   || parent->ts == TREESTATE_SYNCING))))))
    {
        treestate_t state = TREESTATE_NONE;
        if (newts != TREESTATE_NONE && ts == TREESTATE_SYNCING)
        {
            state = TREESTATE_SYNCING;
        }
        else
        {
            state = parent->checkstate();
        }

        parent->treestate(state);
    }

    dts = ts;
}

treestate_t LocalNode::checkstate()
{
    if (type == FILENODE)
        return ts;

    treestate_t state = TREESTATE_SYNCED;
    for (localnode_map::iterator it = children.begin(); it != children.end(); it++)
    {
        if (it->second->ts == TREESTATE_SYNCING)
        {
            state = TREESTATE_SYNCING;
            break;
        }

        if (it->second->ts == TREESTATE_PENDING && state == TREESTATE_SYNCED)
        {
            state = TREESTATE_PENDING;
        }
    }
    return state;
}


// set fsid - assume that an existing assignment of the same fsid is no longer current and revoke
void LocalNode::setSyncedFsid(handle newfsid, fsid_localnode_map& fsidnodes, const LocalPath& fsName, std::unique_ptr<LocalPath> newshortname)
{
    if (fsid_lastSynced_it != fsidnodes.end())
    {
        if (newfsid == fsid_lastSynced && localname == fsName)
        {
            return;
        }

        fsidnodes.erase(fsid_lastSynced_it);
    }

    fsid_lastSynced = newfsid;
    fsidSyncedReused = false;

    // if synced to fs, localname should match exactly (no differences in case/escaping etc)
    if (localname != fsName ||
            !!newshortname != !!slocalname ||
            (newshortname && slocalname && *newshortname != *slocalname))
    {
        // localname must always be set by this function, to maintain parent's child maps
        setnameparent(parent, fsName, move(newshortname));
    }

    // LOG_verbose << "localnode " << this << " fsid " << toHandle(fsid_lastSynced) << " localname " << fsName.toPath() << " parent " << parent;

    if (fsid_lastSynced == UNDEF)
    {
        fsid_lastSynced_it = fsidnodes.end();
    }
    else
    {
        fsid_lastSynced_it = fsidnodes.insert(std::make_pair(fsid_lastSynced, this));
    }

//    assert(localname.empty() || name.empty() || (!parent && parent_dbid == UNDEF) || parent_dbid == 0 ||
//        0 == compareUtf(localname, true, name, false, true));
}

void LocalNode::setScannedFsid(handle newfsid, fsid_localnode_map& fsidnodes, const LocalPath& fsName)
{
    if (fsid_asScanned_it != fsidnodes.end())
    {
        fsidnodes.erase(fsid_asScanned_it);
    }

    fsid_asScanned = newfsid;
    fsidScannedReused = false;

    if (fsid_asScanned == UNDEF)
    {
        fsid_asScanned_it = fsidnodes.end();
    }
    else
    {
        fsid_asScanned_it = fsidnodes.insert(std::make_pair(fsid_asScanned, this));
    }

    assert(fsid_asScanned == UNDEF || 0 == compareUtf(localname, true, fsName, true, true));
}

void LocalNode::setSyncedNodeHandle(NodeHandle h)
{
    if (syncedCloudNodeHandle_it != sync->syncs.localnodeByNodeHandle.end())
    {
        if (h == syncedCloudNodeHandle)
        {
            return;
        }

        assert(syncedCloudNodeHandle_it->first == syncedCloudNodeHandle);

        // too verbose for million-node syncs
        //LOG_verbose << sync->syncname << "removing synced handle " << syncedCloudNodeHandle << " for " << localnodedisplaypath(*sync->syncs.fsaccess);

        sync->syncs.localnodeByNodeHandle.erase(syncedCloudNodeHandle_it);
    }

    syncedCloudNodeHandle = h;

    if (syncedCloudNodeHandle == UNDEF)
    {
        syncedCloudNodeHandle_it = sync->syncs.localnodeByNodeHandle.end();
    }
    else
    {
        // too verbose for million-node syncs
        //LOG_verbose << sync->syncname << "adding synced handle " << syncedCloudNodeHandle << " for " << localnodedisplaypath(*sync->syncs.fsaccess);

        syncedCloudNodeHandle_it = sync->syncs.localnodeByNodeHandle.insert(std::make_pair(syncedCloudNodeHandle, this));
    }

//    assert(localname.empty() || name.empty() || (!parent && parent_dbid == UNDEF) || parent_dbid == 0 ||
//        0 == compareUtf(localname, true, name, false, true));
}

LocalNode::~LocalNode()
{
    if (!sync)
    {
        LOG_err << "LocalNode::init() was never called";
        assert(false);
        return;
    }

    if (!sync->mDestructorRunning && dbid && (
        sync->state() == SYNC_ACTIVE || sync->state() == SYNC_INITIALSCAN))
    {
        sync->statecachedel(this);
    }

    if (!sync->syncs.mExecutingLocallogout)
    {
        // for Locallogout, we will resume syncs and their transfers on re-login.
        // for other cases - single sync cancel, disable etc - transfers are cancelled.
        resetTransfer(nullptr);
    }

    if (sync->dirnotify.get())
    {
        // deactivate corresponding notifyq records
        sync->dirnotify->fsEventq.replaceLocalNodePointers(this, (LocalNode*)~0);
        sync->dirnotify->fsDelayedNetworkEventq.replaceLocalNodePointers(this, (LocalNode*)~0);
    }

    // remove from fsidnode map, if present
    if (fsid_lastSynced_it != sync->syncs.localnodeBySyncedFsid.end())
    {
        sync->syncs.localnodeBySyncedFsid.erase(fsid_lastSynced_it);
    }
    if (fsid_asScanned_it != sync->syncs.localnodeByScannedFsid.end())
    {
        sync->syncs.localnodeByScannedFsid.erase(fsid_asScanned_it);
    }
    if (syncedCloudNodeHandle_it != sync->syncs.localnodeByNodeHandle.end())
    {
        sync->syncs.localnodeByNodeHandle.erase(syncedCloudNodeHandle_it);
    }

    sync->syncs.totalLocalNodes--;

    if (type != TYPE_UNKNOWN)
    {
        sync->localnodes[type]--;    // todo: make sure we are not using the larger types and overflowing the buffer
    }

    // remove parent association
    if (parent)
    {
        setnameparent(nullptr, LocalPath(), nullptr);
    }

    deleteChildren();
}

void LocalNode::deleteChildren()
{
    for (localnode_map::iterator it = children.begin(); it != children.end(); )
    {
        // the destructor removes the child from our `children` map
        delete it++->second;
    }
    assert(children.empty());
}


bool LocalNode::conflictsDetected() const
{
    return conflicts != TREE_RESOLVED;
}

bool LocalNode::isAbove(const LocalNode& other) const
{
    return other.isBelow(*this);
}

bool LocalNode::isBelow(const LocalNode& other) const
{
    for (auto* node = parent; node; node = node->parent)
    {
        if (node == &other)
        {
            return true;
        }
    }

    return false;
}

LocalPath LocalNode::getLocalPath() const
{
    LocalPath lp;
    getlocalpath(lp);
    return lp;
}

void LocalNode::getlocalpath(LocalPath& path) const
{
    path.erase();

    for (const LocalNode* l = this; l != nullptr; l = l->parent)
    {
        assert(!l->parent || l->parent->sync == sync);

        // sync root has absolute path, the rest are just their leafname
        path.prependWithSeparator(l->localname);
    }
}

string LocalNode::localnodedisplaypath(FileSystemAccess& fsa) const
{
    LocalPath local;
    getlocalpath(local);
    return local.toPath(fsa);
}

string LocalNode::getCloudPath() const
{
    string path;

    for (const LocalNode* l = this; l != nullptr; l = l->parent)
    {
        string name;

        CloudNode cn;
        string fullpath;
        if (sync->syncs.lookupCloudNode(l->syncedCloudNodeHandle, cn, l->parent ? nullptr : &fullpath,
            nullptr, nullptr, nullptr, Syncs::LATEST_VERSION))
        {
            name = cn.name;
        }
        else
        {
            name = localname.toName(*sync->syncs.fsaccess);
        }

        assert(!l->parent || l->parent->sync == sync);
        path = (l->parent ? name : fullpath) + "/" + path;
    }
    return path;
}

// locate child by localname or slocalname
LocalNode* LocalNode::childbyname(LocalPath* localname)
{
    localnode_map::iterator it;

    if (!localname || ((it = children.find(localname)) == children.end() && (it = schildren.find(localname)) == schildren.end()))
    {
        return NULL;
    }

    return it->second;
}

LocalNode* LocalNode::findChildWithSyncedNodeHandle(NodeHandle h)
{
    for (auto& c : children)
    {
        if (c.second->syncedCloudNodeHandle == h)
        {
            return c.second;
        }
    }
    return nullptr;
}

FSNode LocalNode::getLastSyncedFSDetails()
{
    assert(fsid_lastSynced != UNDEF);

    FSNode n;
    n.localname = localname;
    n.shortname = slocalname ? make_unique<LocalPath>(*slocalname): nullptr;
    n.type = type;
    n.fsid = fsid_lastSynced;
    n.isSymlink = false;  // todo: store localndoes for symlinks but don't use them?
    n.fingerprint = syncedFingerprint;
    return n;
}


FSNode LocalNode::getScannedFSDetails()
{
    FSNode n;
    n.localname = localname;
    n.shortname = slocalname ? make_unique<LocalPath>(*slocalname): nullptr;
    n.type = type;
    n.fsid = fsid_asScanned;
    n.isSymlink = false;  // todo: store localndoes for symlinks but don't use them?
    n.fingerprint = scannedFingerprint;
    assert(scannedFingerprint.isvalid || type != FILENODE);
    return n;
}

void LocalNode::queueClientUpload(shared_ptr<SyncUpload_inClient> upload)
{
    resetTransfer(upload);

    sync->syncs.queueClient([upload](MegaClient& mc, DBTableTransactionCommitter& committer)
        {
            mc.nextreqtag();
            mc.startxfer(PUT, upload.get(), committer);
        });

}

void LocalNode::queueClientDownload(shared_ptr<SyncDownload_inClient> download)
{
    resetTransfer(download);

    sync->syncs.queueClient([download](MegaClient& mc, DBTableTransactionCommitter& committer)
        {
            mc.nextreqtag();
            mc.startxfer(GET, download.get(), committer);
        });

}

void LocalNode::resetTransfer(shared_ptr<SyncTransfer_inClient> p)
{
    if (transferSP)
    {
        // this flag allows in-progress transfers to self-cancel
        transferSP->wasRequesterAbandoned = true;

        // also queue an operation on the client thread to cancel it if it's queued
        auto tsp = transferSP;
        sync->syncs.queueClient([tsp](MegaClient& mc, DBTableTransactionCommitter& committer)
            {
                mc.nextreqtag();
                mc.stopxfer(tsp.get(), &committer);
            });
    }

    if (p) p->selfKeepAlive = p;
    transferSP = move(p);
}

void LocalNode::checkTransferCompleted()
{
    if (auto upload = dynamic_cast<SyncUpload_inClient*>(transferSP.get()))
    {
        if (transferSP->wasTerminated ||
            (transferSP->wasCompleted && upload->wasPutnodesCompleted))
        {
            resetTransfer(nullptr);
        }
    }
    else if (transferSP)
    {
        if (transferSP->wasTerminated)
        {
            resetTransfer(nullptr);
        }
        else if (transferSP->wasCompleted)
        {
            // We don't clear the pointer here, becuase we need it around
            // for the next recursiveSync node visit here, which will move
            // and rename the downloaded file to the corresponding
            // current location of this LocalNode (ie, copes with
            // localnodes that moved during the download.
        }
    }
}

void LocalNode::updateTransferLocalname()
{
    if (transferSP)
    {
        transferSP->setLocalname(getLocalPath());
    }
}

void LocalNode::transferResetUnlessMatched(direction_t dir, const FileFingerprint& fingerprint)
{
    // todo: should we be more accurate than just fingerprint?
    if (transferSP && (
        dir != (dynamic_cast<SyncUpload_inClient*>(transferSP.get()) ? PUT : GET) ||
        !(transferSP->fingerprint() == fingerprint)))
    {
        LOG_debug << sync->syncname << "Cancelling superceded transfer of " << transferSP->getLocalname().toPath();
        resetTransfer(nullptr);
    }
}

void SyncTransfer_inClient::terminated()
{
    //todo: put this somewhere
    //localNode.sync->mUnifiedSync.mNextHeartbeat->adjustTransferCounts(-1, 0, size, 0);

    File::terminated();

    wasTerminated = true;
    selfKeepAlive.reset();  // deletes this object! (if abandoned by sync)
}

void SyncTransfer_inClient::completed(Transfer* t, putsource_t source)
{
    //todo: put this somewhere
    //localNode.sync->mUnifiedSync.mNextHeartbeat->adjustTransferCounts(-1, 0, 0, size);

    File::completed(t, source);

    wasCompleted = true;
    selfKeepAlive.reset();  // deletes this object! (if abandoned by sync)
}

SyncUpload_inClient::SyncUpload_inClient(NodeHandle targetFolder, const LocalPath& fullPath, const string& nodeName, const FileFingerprint& ff)
{
    *static_cast<FileFingerprint*>(this) = ff;

    // normalized name (UTF-8 with unescaped special chars)
    // todo: we did unescape them though?
    name = nodeName;

    // setting the full path means it works like a normal non-sync transfer
    setLocalname(fullPath);

    h = targetFolder;

    hprivate = false;
    hforeign = false;
    syncxfer = true;
    temporaryfile = false;
    chatauth = nullptr;
    transfer = nullptr;
    tag = 0;

    //todo: put this somewhere
    //localNode.sync->mUnifiedSync.mNextHeartbeat->adjustTransferCounts(1, 0, size, 0);
}

SyncUpload_inClient::~SyncUpload_inClient()
{
    if (!wasTerminated && !wasCompleted)
    {
        assert(wasRequesterAbandoned);
        transfer = nullptr;  // don't try to remove File from Transfer from the wrong thread
    }
}

void SyncUpload_inClient::prepare(FileSystemAccess&)
{
    transfer->localfilename = getLocalname();

    // is this transfer in progress? update file's filename.
    if (transfer->slot && transfer->slot->fa && !transfer->slot->fa->nonblocking_localname.empty())
    {
        transfer->slot->fa->updatelocalname(transfer->localfilename, false);
    }

    //todo: localNode.treestate(TREESTATE_SYNCING);
}



// serialize/unserialize the following LocalNode properties:
// - type/size
// - fsid
// - parent LocalNode's dbid
// - corresponding Node handle
// - local name
// - fingerprint crc/mtime (filenodes only)
bool LocalNode::serialize(string* d)
{
#ifdef DEBUG
    if (fsid_lastSynced != UNDEF)
    {
        LocalPath localpath = getLocalPath();
        auto fa = sync->syncs.fsaccess->newfileaccess(false);
        if (fa->fopen(localpath))  // exists, is file
        {
            auto sn = sync->syncs.fsaccess->fsShortname(localpath);
            assert(!localname.empty() &&
                ((!slocalname && (!sn || localname == *sn)) ||
                    (slocalname && sn && !slocalname->empty() && *slocalname != localname && *slocalname == *sn)));
        }
    }
#endif

    assert(type != TYPE_UNKNOWN);
    assert(type != FILENODE || syncedFingerprint.isvalid || scannedFingerprint.isvalid);

    // we need size even if not synced, now that scannedFingerprint is separate, for serialization compatibility
    auto size = syncedFingerprint.isvalid ? syncedFingerprint.size : scannedFingerprint.size;
    if (size < 0) size = 0;

    CacheableWriter w(*d);
    w.serializei64(type ? -type : size);
    w.serializehandle(fsid_lastSynced);
    w.serializeu32(parent ? parent->dbid : 0);
    w.serializenodehandle(syncedCloudNodeHandle.as8byte());
    w.serializestring(localname.platformEncoded());
    if (type == FILENODE)
    {
        if (syncedFingerprint.isvalid)
        {
            w.serializebinary((byte*)syncedFingerprint.crc.data(), sizeof(syncedFingerprint.crc));
            w.serializecompressed64(syncedFingerprint.mtime);
        }
        else
        {
            static FileFingerprint zeroFingerprint;
            w.serializebinary((byte*)zeroFingerprint.crc.data(), sizeof(zeroFingerprint.crc));
            w.serializecompressed64(zeroFingerprint.mtime);
        }
    }
    w.serializebyte(mSyncable);
    w.serializeexpansionflags(1);  // first flag indicates we are storing slocalname.  Storing it is much, much faster than looking it up on startup.
    auto tmpstr = slocalname ? slocalname->platformEncoded() : string();
    w.serializepstr(slocalname ? &tmpstr : nullptr);

#ifdef DEBUG
    // just check deserializing, real quick, only in debug
    string testread = w.dest;
    auto test = LocalNode::unserialize(sync, &testread);
    assert(test->localname == localname);
    assert((test->slocalname && slocalname) || (!test->slocalname && !slocalname));
    assert(!test->slocalname || *test->slocalname == *slocalname);
#endif

    return true;
}

unique_ptr<LocalNode> LocalNode::unserialize(Sync* sync, const string* d)
{
    if (d->size() < sizeof(m_off_t)         // type/size combo
                  + sizeof(handle)          // fsid
                  + sizeof(uint32_t)        // parent dbid
                  + MegaClient::NODEHANDLE  // handle
                  + sizeof(short))          // localname length
    {
        LOG_err << "LocalNode unserialization failed - short data";
        return NULL;
    }

    CacheableReader r(*d);

    nodetype_t type;
    m_off_t size;

    if (!r.unserializei64(size)) return nullptr;

    if (size < 0 && size >= -FOLDERNODE)
    {
        // will any compiler optimize this to a const assignment?
        type = (nodetype_t)-size;
        size = 0;
    }
    else
    {
        type = FILENODE;
    }

    handle fsid;
    uint32_t parent_dbid;
    handle h = 0;
    string localname, shortname;
    uint64_t mtime = 0;
    int32_t crc[4];
    memset(crc, 0, sizeof crc);
    byte syncable = 1;
    unsigned char expansionflags[8] = { 0 };

    if (!r.unserializehandle(fsid) ||
        !r.unserializeu32(parent_dbid) ||
        !r.unserializenodehandle(h) ||
        !r.unserializestring(localname) ||
        (type == FILENODE && !r.unserializebinary((byte*)crc, sizeof(crc))) ||
        (type == FILENODE && !r.unserializecompressed64(mtime)) ||
        (r.hasdataleft() && !r.unserializebyte(syncable)) ||
        (r.hasdataleft() && !r.unserializeexpansionflags(expansionflags, 1)) ||
        (expansionflags[0] && !r.unserializecstr(shortname, false)))
    {
        LOG_err << "LocalNode unserialization failed at field " << r.fieldnum;
        assert(false);
        return nullptr;
    }
    assert(!r.hasdataleft());

    unique_ptr<LocalNode> l(new LocalNode());

    l->type = type;
    l->syncedFingerprint.size = size;

    l->parent_dbid = parent_dbid;

    l->fsid_lastSynced = fsid;
    l->fsid_lastSynced_it = sync->syncs.localnodeBySyncedFsid.end();
    l->fsid_asScanned = UNDEF;
    l->fsid_asScanned_it = sync->syncs.localnodeByScannedFsid.end();

    l->localname = LocalPath::fromPlatformEncoded(localname);
    l->slocalname.reset(shortname.empty() ? nullptr : new LocalPath(LocalPath::fromPlatformEncoded(shortname)));
    l->slocalname_in_db = 0 != expansionflags[0];

    memcpy(l->syncedFingerprint.crc.data(), crc, sizeof crc);
    l->syncedFingerprint.mtime = mtime;
    l->syncedFingerprint.isvalid = mtime != 0;  // previously we scanned and created the LocalNode, but we had not set syncedFingerprint

    l->syncedCloudNodeHandle.set6byte(h);
    l->syncedCloudNodeHandle_it = sync->syncs.localnodeByNodeHandle.end();

//    l->node = sync->client->nodebyhandle(h);
    l->parent = nullptr;
    l->sync = sync;
    l->mSyncable = syncable == 1;

    return l;
}

#ifdef USE_INOTIFY

LocalNode::WatchHandle::WatchHandle()
  : mEntry(mSentinel.end())
{
}

LocalNode::WatchHandle::~WatchHandle()
{
    operator=(nullptr);
}

auto LocalNode::WatchHandle::operator=(WatchMapIterator entry) -> WatchHandle&
{
    if (mEntry == entry) return *this;

    operator=(nullptr);
    mEntry = entry;

    return *this;
}

auto LocalNode::WatchHandle::operator=(std::nullptr_t) -> WatchHandle&
{
    if (mEntry == mSentinel.end()) return *this;

    auto& node = *mEntry->second.first;
    auto& sync = *node.sync;
    auto& notifier = static_cast<PosixDirNotify&>(*sync.dirnotify);

    notifier.removeWatch(mEntry);
    mEntry = mSentinel.end();
}

bool LocalNode::WatchHandle::operator==(handle fsid) const
{
    if (mEntry == mSentinel.end()) return false;

    return fsid == mEntry->second.second;
}

bool LocalNode::watch(const LocalPath& path, handle fsid)
{
    // Do we need to (re)create a watch?
    if (mWatchHandle == fsid) return true;

    // Get our hands on the notifier.
    auto& notifier = static_cast<PosixDirNotify&>(*sync->dirnotify);

    // Add the watch.
    auto result = notifier.addWatch(*this, path, fsid);

    // Were we able to add the watch?
    if (result.second)
    {
        // Yup so assign the handle.
        mWatchHandle = result.first;
    }
    else
    {
        // Make sure any existing watch is invalidated.
        mWatchHandle = nullptr;
    }

    return result.second;
}

WatchMap LocalNode::WatchHandle::mSentinel;

#else // USE_INOTIFY

bool LocalNode::watch(const LocalPath&, handle)
{
    // Only inotify requires us to create watches for each node.
    return true;
}

#endif // ! USE_INOTIFY

void LocalNode::clearFilters()
{
    // Only for directories.
    assert(type == FOLDERNODE);

    // Purge all defined filter rules.
    mFilterChain.clear();

    // Reset ignore file state.
    setLoadPending(false);

    // Re-examine this subtree.
    setScanAgain(false, true, true, 0);
    setSyncAgain(false, true, true);
}

bool LocalNode::loadFilters(const LocalPath& path)
{
    // Only meaningful for directories.
    assert(type == FOLDERNODE);

    // Convenience.
    auto& fsAccess = *sync->syncs.fsaccess;

    // Try and load the ignore file.
    auto result = mFilterChain.load(fsAccess, path);

    // Ignore file hasn't changed.
    if (result == FLR_SKIPPED)
        return !mLoadPending;

    auto failed = result == FLR_FAILED;

    // Did the load fail?
    if (failed)
    {
        // Let the engine know there's been a load failure.
        sync->syncs.ignoreFileLoadFailure(*sync, path);

        // Clear the filter state.
        mFilterChain.clear();
    }

    // Update our load pending state.
    setLoadPending(failed);

    // Re-examine this subtree.
    setScanAgain(false, true, true, 0);
    setSyncAgain(false, true, true);

    return !failed;
}

bool LocalNode::isExcluded(RemotePathPair namePath, nodetype_t type, bool inherited) const
{
    // This specialization only makes sense for directories.
    assert(this->type == FOLDERNODE);

    // Check whether the file is excluded by any filters.
    for (auto* node = this; node; node = node->parent)
    {
        // Sanity: We should never have been called if either of these are true.
        assert(!node->mExcluded);
        assert(!node->mRecomputeExclusionState);

        // Should we only consider inheritable filter rules?
        inherited = inherited || node != this;

        // Check for a filter match.
        auto result = node->mFilterChain.match(namePath, type, inherited);

        // Was the file matched by any filters?
        if (result.matched)
            return !result.included;

        // Compute the node's cloud name.
        auto name = node->localname.toName(*sync->syncs.fsaccess);

        // Update path so that it's applicable to the next node's path filters.
        namePath.second.prependWithSeparator(name);
    }

    // File's included.
    return false;
}

bool LocalNode::isExcluded(const RemotePathPair&, m_off_t size) const
{
    // Specialization only meaningful for directories.
    assert(type == FOLDERNODE);

    // Consider files of unknown size included.
    if (size < 0)
        return false;

    // Check whether this file is excluded by any size filters.
    for (auto* node = this; node; node = node->parent)
    {
        // Sanity: We should never be called if either of these is true.
        assert(!node->mExcluded);
        assert(!node->mRecomputeExclusionState);

        // Check for a filter match.
        auto result = node->mFilterChain.match(size);

        // Was the file matched by any filters?
        if (result.matched)
            return !result.included;
    }

    // File's included.
    return false;
}

void LocalNode::setLoadPending(bool pending)
{
    // Only meaningful for directories.
    assert(type == FOLDERNODE);

    // Do we really need to update our children?
    if (!mLoadPending)
    {
        // Tell our children they need to recompute their state.
        for (auto& childIt : children)
            childIt.second->setRecomputeExclusionState();
    }

    // Apply new pending state.
    mLoadPending = pending;
}

void LocalNode::setRecomputeExclusionState()
{
    if (mRecomputeExclusionState)
        return;

    mExcluded = false;
    mRecomputeExclusionState = true;

    if (type == FILENODE)
        return;

    list<LocalNode*> pending(1, this);

    while (!pending.empty())
    {
        auto& node = *pending.front();

        for (auto& childIt : node.children)
        {
            auto& child = *childIt.second;

            if (child.mRecomputeExclusionState)
                continue;

            child.mExcluded = false;
            child.mRecomputeExclusionState = true;

            if (child.type == FOLDERNODE)
                pending.emplace_back(&child);
        }

        pending.pop_front();
    }
}

bool LocalNode::hasParentWithPendingLoad() const
{
    for (auto* node = parent; node; node = node->parent)
    {
        if (node->mLoadPending)
            return true;
    }

    return false;
}

bool LocalNode::hasPendingLoad() const
{
    return mLoadPending || hasParentWithPendingLoad();
}


bool LocalNode::ignoreFileChanged(const FileFingerprint& fingerprint) const
{
    // Only meaningful for ignore files.
    assert(mIsIgnoreFile);

    return parent->mFilterChain.changed(fingerprint);
}

void LocalNode::ignoreFileDownloading()
{
    // Only meaningful for ignore files.
    assert(mIsIgnoreFile);

    // Let the parent know it has a pending load.
    parent->setLoadPending(true);
}

bool LocalNode::ignoreFileLoad(const LocalPath& path)
{
    // Only meaningful for ignore files.
    assert(mIsIgnoreFile);

    return parent->loadFilters(path);
}

void LocalNode::ignoreFileRemoved()
{
    // Only meaningful for ignore files.
    assert(mIsIgnoreFile);

    parent->clearFilters();
}

// Query whether a file is excluded by this node or one of its parents.
template<typename PathType>
typename std::enable_if<IsPath<PathType>::value, int>::type
LocalNode::isExcluded(const PathType& path, nodetype_t type, m_off_t size) const
{
    // This specialization is only meaningful for directories.
    assert(this->type == FOLDERNODE);

    // We can't determine our child's exclusion state if we don't know our own.
    if (mRecomputeExclusionState)
        return 0;

    // Our children are excluded if we are.
    if (mExcluded)
        return -1;

    // Children of unknown type are considered excluded.
    if (type == TYPE_UNKNOWN)
        return -1;

    // Tracks whether the child is our own ignore file.
    auto isIgnoreFile = false;

    // Is the child a file?
    if (type == FILENODE)
    {
        // Is the child our ignore file?
        isIgnoreFile = path == IGNORE_FILE_NAME;
    }

    // We can't know the child's state unless our filters are current.
    if (mLoadPending && !isIgnoreFile)
        return 0;

    // Computed cloud name and relative cloud path.
    RemotePathPair namePath;

    // Current path component.
    PathType component;

    // Check if any intermediary path components are excluded.
    for (size_t index = 0; path.nextPathComponent(index, component); )
    {
        // Compute cloud name.
        namePath.first = component.toName(*sync->syncs.fsaccess);

        // Compute relative cloud path.
        namePath.second.appendWithSeparator(namePath.first, false);

        // Have we hit the final path component?
        if (!path.hasNextPathComponent(index))
            break;

        // Is this path component excluded?
        if (isExcluded(namePath, FOLDERNODE, false))
            return -1;
    }

    // Which node we should start our search from.
    auto* node = this;

    // Does the final path component represent a file?
    if (type == FILENODE)
    {
        // Does it represent this node's ignore file?
        if (namePath.second == IGNORE_FILE_NAME)
        {
            // Then start the lookup from our parent so that the ignore file
            // isn't subject to its own rules.
            node = parent;

            // If we're the root then the ignore file must be included.
            if (!node)
                return 1;
        }

        // Is the file excluded by any size filters?
        if (node->isExcluded(namePath, size))
            return -1;
    }

    // Is the file excluded by any name filters?
    if (node->isExcluded(namePath, type, node != this))
        return -1;

    // File's included.
    return 1;
}

// Make sure we instantiate the two types.  Jenkins gcc can't handle this in the header.
template int LocalNode::isExcluded(const LocalPath& path, nodetype_t type, m_off_t size) const;
template int LocalNode::isExcluded(const RemotePath& path, nodetype_t type, m_off_t size) const;


int LocalNode::isExcluded(const string& name, nodetype_t type, m_off_t size) const
{
    assert(this->type == FOLDERNODE);

    // Consider providing a specialized implementation to avoid conversion.
    auto fsAccess = sync->syncs.fsaccess.get();
    auto fsType = sync->mFilesystemType;
    auto localname = LocalPath::fromName(name, *fsAccess, fsType);

    return isExcluded(localname, type, size);
}

int LocalNode::isExcluded() const
{
    if (mRecomputeExclusionState)
        return 0;

    if (mExcluded)
        return -1;

    return 1;
}

bool LocalNode::isIgnoreFile() const
{
    return mIsIgnoreFile;
}

bool LocalNode::recomputeExclusionState()
{
    // We should never be asked to recompute the root's exclusion state.
    assert(parent);

    // Only recompute the state if it's necessary.
    if (!mRecomputeExclusionState)
        return false;

    auto excluded = parent->isExcluded(localname, type);

    mExcluded = excluded < 0;
    mRecomputeExclusionState = excluded == 0;

    return !mRecomputeExclusionState;
}

#endif // ENABLE_SYNC

void Fingerprints::newnode(Node* n)
{
    if (n->type == FILENODE)
    {
        n->fingerprint_it = mFingerprints.end();
    }
}

void Fingerprints::add(Node* n)
{
    assert(n->fingerprint_it == mFingerprints.end());
    if (n->type == FILENODE)
    {
        n->fingerprint_it = mFingerprints.insert(n);
        mSumSizes += n->size;
    }
}

void Fingerprints::remove(Node* n)
{
    if (n->type == FILENODE && n->fingerprint_it != mFingerprints.end())
    {
        mSumSizes -= n->size;
        mFingerprints.erase(n->fingerprint_it);
        n->fingerprint_it = mFingerprints.end();
    }
}

void Fingerprints::clear()
{
    mFingerprints.clear();
    mSumSizes = 0;
}

m_off_t Fingerprints::getSumSizes()
{
    return mSumSizes;
}

Node* Fingerprints::nodebyfingerprint(FileFingerprint* fingerprint)
{
    fingerprint_set::iterator it = mFingerprints.find(fingerprint);
    return it == mFingerprints.end() ? nullptr : static_cast<Node*>(*it);
}

node_vector *Fingerprints::nodesbyfingerprint(FileFingerprint* fingerprint)
{
    node_vector *nodes = new node_vector();
    auto p = mFingerprints.equal_range(fingerprint);
    for (iterator it = p.first; it != p.second; ++it)
    {
        nodes->push_back(static_cast<Node*>(*it));
    }
    return nodes;
}

unique_ptr<FSNode> FSNode::fromFOpened(FileAccess& fa, const LocalPath& fullPath, FileSystemAccess& fsa)
{
    unique_ptr<FSNode> result(new FSNode);
    result->type = fa.type;
    result->fsid = fa.fsidvalid ? fa.fsid : UNDEF;
    result->isSymlink = fa.mIsSymLink;
    result->fingerprint.mtime = fa.mtime;
    result->fingerprint.size = fa.size;

    result->localname = fullPath.leafName();

    if (auto sn = fsa.fsShortname(fullPath))
    {
        if (*sn != result->localname)
        {
            result->shortname = std::move(sn);
        }
    }
    return result;
}


CloudNode::CloudNode(const Node& n)
    : name(n.displayname())
    , type(n.type)
    , handle(n.nodeHandle())
    , parentHandle(n.parent ? n.parent->nodeHandle() : NodeHandle())
    , parentType(n.parent ? n.parent->type : TYPE_UNKNOWN)
    , fingerprint(n.fingerprint())
{}

bool CloudNode::isIgnoreFile() const
{
    return type == FILENODE && name == IGNORE_FILE_NAME;
}

} // namespace
