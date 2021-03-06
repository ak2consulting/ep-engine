/* -*- Mode: C++; tab-width: 4; c-basic-offset: 4; indent-tabs-mode: nil -*- */
#include "config.h"
#include <functional>

#include "vbucket.hh"
#include "ep_engine.h"

VBucketFilter VBucketFilter::filter_diff(const VBucketFilter &other) const {
    std::vector<uint16_t> tmp(acceptable.size() + other.size());
    std::vector<uint16_t>::iterator end;
    end = std::set_symmetric_difference(acceptable.begin(),
                                        acceptable.end(),
                                        other.acceptable.begin(),
                                        other.acceptable.end(),
                                        tmp.begin());
    return VBucketFilter(std::vector<uint16_t>(tmp.begin(), end));
}

VBucketFilter VBucketFilter::filter_intersection(const VBucketFilter &other) const {
    std::vector<uint16_t> tmp(acceptable.size() + other.size());
    std::vector<uint16_t>::iterator end;

    end = std::set_intersection(acceptable.begin(), acceptable.end(),
                                other.acceptable.begin(), other.acceptable.end(),
                                tmp.begin());
    return VBucketFilter(std::vector<uint16_t>(tmp.begin(), end));
}

std::ostream& operator <<(std::ostream &out, const VBucketFilter &filter)
{
    bool needcomma = false;
    std::vector<uint16_t>::const_iterator it;

    if (filter.acceptable.empty()) {
        out << "{ empty }";
    } else {
        out << "{ ";
        for (it = filter.acceptable.begin();
             it != filter.acceptable.end();
             ++it) {
            if (needcomma) {
                out << ", ";
            }
            out << *it;
            needcomma = true;
        }
        out << " }";
    }

     return out;
 }

const vbucket_state_t VBucket::ACTIVE = static_cast<vbucket_state_t>(htonl(active));
const vbucket_state_t VBucket::REPLICA = static_cast<vbucket_state_t>(htonl(replica));
const vbucket_state_t VBucket::PENDING = static_cast<vbucket_state_t>(htonl(pending));
const vbucket_state_t VBucket::DEAD = static_cast<vbucket_state_t>(htonl(dead));

void VBucket::fireAllOps(SERVER_HANDLE_V1 *sapi, ENGINE_ERROR_CODE code) {
    if (pendingOpsStart > 0) {
        hrtime_t now = gethrtime();
        if (now > pendingOpsStart) {
            hrtime_t d = (now - pendingOpsStart) / 1000;
            stats.pendingOpsHisto.add(d);
            stats.pendingOpsMaxDuration.setIfBigger(d);
        }
    }
    pendingOpsStart = 0;
    stats.pendingOps.decr(pendingOps.size());
    stats.pendingOpsMax.setIfBigger(pendingOps.size());

    std::for_each(pendingOps.begin(), pendingOps.end(),
                  std::bind2nd(std::ptr_fun((NOTIFY_IO_COMPLETE_T)sapi->cookie->notify_io_complete), code));
    pendingOps.clear();

    getLogger()->log(EXTENSION_LOG_INFO, NULL,
                     "Fired pendings ops for vbucket %d in state %s\n",
                     id, VBucket::toString(state));
}

void VBucket::fireAllOps(SERVER_HANDLE_V1 *sapi) {
    LockHolder lh(pendingOpLock);

    if (state == active) {
        fireAllOps(sapi, ENGINE_SUCCESS);
    } else if (state == pending) {
        // Nothing
    } else {
        fireAllOps(sapi, ENGINE_NOT_MY_VBUCKET);
    }
}

void VBucket::setState(vbucket_state_t to, SERVER_HANDLE_V1 *sapi) {
    assert(sapi);
    vbucket_state_t oldstate(state);

    getLogger()->log(EXTENSION_LOG_DEBUG, NULL,
                     "transitioning vbucket %d from %s to %s\n",
                     id, VBucket::toString(oldstate), VBucket::toString(to));

    state = to;
}
