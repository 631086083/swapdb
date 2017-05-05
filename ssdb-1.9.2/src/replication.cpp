//
// Created by zts on 17-4-27.
//
#include "replication.h"
#include <util/thread.h>
#include <net/link.h>
#include "serv.h"

extern "C" {
#include <redis/rdb.h>
#include <redis/zmalloc.h>
#include <redis/lzf.h>
};


void send_error_to_redis(Link *link);

std::string ssdb_save_len(uint64_t len);

void moveBuffer(Buffer *dst, Buffer *src);

void saveStrToBuffer(Buffer *buffer, const Bytes &fit);

ReplicationWorker::ReplicationWorker(const std::string &name) {
    this->name = name;
}

ReplicationWorker::~ReplicationWorker() {

}

void ReplicationWorker::init() {
    log_debug("%s %d init", this->name.c_str(), this->id);
}

int ReplicationWorker::proc(ReplicationJob *job) {

    SSDBServer *serv = job->serv;
    HostAndPort hnp = job->hnp;
    Link *master_link = job->upstreamRedis;
    const leveldb::Snapshot *snapshot = nullptr;

    log_info("[ReplicationWorker] send snapshot to %s:%d start!", hnp.ip.c_str(), hnp.port);
    {
        Locking<Mutex> l(&serv->replicMutex);
        if (serv->replicSnapshot == nullptr) {
            log_error("snapshot is null, maybe rr_make_snapshot not receive or error!");

            reportError(job);

            return -1;
        }
        snapshot = serv->replicSnapshot;
    }

    std::unique_ptr<Iterator> fit = std::unique_ptr<Iterator>(serv->ssdb->iterator("", "", -1, snapshot));

    Link *ssdb_slave_link = Link::connect((hnp.ip).c_str(), hnp.port);
    if (ssdb_slave_link == nullptr) {
        log_error("fail to connect to slave ssdb! ip[%s] port[%d]", hnp.ip.c_str(), hnp.port);
        log_debug("replic send snapshot failed!");

        reportError(job);

        return -1;
    }

    ssdb_slave_link->noblock(false);
    ssdb_slave_link->send(std::vector<std::string>({"sync150"}));
    ssdb_slave_link->write();
    ssdb_slave_link->response();
    ssdb_slave_link->noblock(true);


    log_debug("[ReplicationWorker] prepare for event loop");
    unique_ptr<Fdevents> fdes = unique_ptr<Fdevents>(new Fdevents());

    fdes->set(master_link->fd(), FDEVENT_IN, 1, master_link); //open evin
    master_link->noblock(true);

    const Fdevents::events_t *events;
    ready_list_t ready_list;
    ready_list_t ready_list_2;
    ready_list_t::iterator it;

    std::unique_ptr<Buffer> buffer = std::unique_ptr<Buffer>(new Buffer(8 * 1024));

    uint64_t sendBytes = 0;

    while (!job->quit) {
        ready_list.swap(ready_list_2);
        ready_list_2.clear();

        if (!ready_list.empty()) {
            // ready_list not empty, so we should return immediately
            events = fdes->wait(0);
        } else {
            events = fdes->wait(5);
        }

        if (events == nullptr) {
            log_fatal("events.wait error: %s", strerror(errno));

            reportError(job);
            delete ssdb_slave_link;

            return -1;
        }

        for (int i = 0; i < (int) events->size(); i++) {
            //processing
            const Fdevent *fde = events->at(i);
            Link *link = (Link *) fde->data.ptr;
            if (fde->events & FDEVENT_IN) {
                ready_list.push_back(link);
                if (link->error()) {
                    continue;
                }
                int len = link->read();
                if (len <= 0) {
                    log_debug("fd: %d, read: %d, delete link, e:%d, f:%d", link->fd(), len, fde->events, fde->s_flags);
                    link->mark_error();
                    continue;
                }
            }
            if (fde->events & FDEVENT_OUT) {
                if (link->output->empty()) {
                    fdes->clr(link->fd(), FDEVENT_OUT);
                    continue;
                }

                ready_list.push_back(link); //push into ready_list
                if (link->error()) {
                    continue;
                }
                int len = link->write();
                if (len <= 0) {
//                if (len < 0) {
                    log_debug("fd: %d, write: %d, delete link, e:%d, f:%d", link->fd(), len, fde->events, fde->s_flags);
                    link->mark_error();
                    continue;
                } else if (link == ssdb_slave_link) {
                    sendBytes = sendBytes + len;
                }
                if (link->output->empty()) {
                    fdes->clr(link->fd(), FDEVENT_OUT);
                }
            }
        }

        for (it = ready_list.begin(); it != ready_list.end(); it++) {
            Link *link = *it;
            if (link->error()) {
                log_warn("fd: %d, link broken, address:%lld", link->fd(), link);

                if (link == master_link) {
                    log_info("link to redis broken");
                } else if (link == ssdb_slave_link) {
                    log_info("link to slave ssdb broken");
                    send_error_to_redis(master_link);
                } else {
                    log_info("?????????????????????????????????WTF????????????????????????????????????????????????");
                }

                fdes->del(ssdb_slave_link->fd());
                fdes->del(master_link->fd());

                delete ssdb_slave_link;
                delete master_link;
                job->upstreamRedis = nullptr;

                {
                    //update replic stats
                    Locking<Mutex> l(&serv->replicMutex);
                    serv->replicNumFailed++;
                    if (serv->replicNumStarted == (serv->replicNumFinished + serv->replicNumFailed)) {
                        serv->replicState = REPLIC_END;
                    }
                }

                return -1;
            }
        }

        if (ssdb_slave_link->output->size() > (2 * 1024 * 1024)) {
//            ssdb_slave_link->write();
            log_debug("delay for output buffer write slow~");
            usleep(500);
            continue;
        }

        bool finish = true;
        while (fit->next()) {
            saveStrToBuffer(buffer.get(), fit->key());
            saveStrToBuffer(buffer.get(), fit->val());

            if (buffer->size() > (512 * 1024)) {
                saveStrToBuffer(ssdb_slave_link->output, "mset");
                moveBuffer(ssdb_slave_link->output, buffer.get());
                int len = ssdb_slave_link->write();
                if (len > 0) { sendBytes = sendBytes + len; }

                if (!ssdb_slave_link->output->empty()) {
                    fdes->set(ssdb_slave_link->fd(), FDEVENT_OUT, 1, ssdb_slave_link);
                }
                finish = false;
                break;
            }
        }

        if (finish) {
            if (!buffer->empty()) {
                saveStrToBuffer(ssdb_slave_link->output, "mset");
                moveBuffer(ssdb_slave_link->output, buffer.get());
                int len = ssdb_slave_link->write();
                if (len > 0) { sendBytes = sendBytes + len; }

                if (!ssdb_slave_link->output->empty()) {
                    fdes->set(ssdb_slave_link->fd(), FDEVENT_OUT, 1, ssdb_slave_link);
                }
            }

            if (!ssdb_slave_link->output->empty()) {
                log_debug("wait for output buffer empty~");
                continue; //wait for buffer empty
            } else {
                break;
            }
        }
    }

    {
        //del from event loop
        fdes->del(ssdb_slave_link->fd());
        fdes->del(master_link->fd());
    }

    {
        //write "complete" to slave_ssdb
        ssdb_slave_link->noblock(false);
        saveStrToBuffer(ssdb_slave_link->output, "complete");
        int len = ssdb_slave_link->write();
        if (len > 0) { sendBytes = sendBytes + len; }

        ssdb_slave_link->read(); //todo error process
        delete ssdb_slave_link;
    }

    {
        //update replic stats
        Locking<Mutex> l(&serv->replicMutex);
        serv->replicNumFinished++;
        if (serv->replicNumStarted == (serv->replicNumFinished + serv->replicNumFailed)) {
            serv->replicState = REPLIC_END;
        }
    }

    log_info("[ReplicationWorker] send snapshot to %s:%d finished!", hnp.ip.c_str(), hnp.port);
    log_debug("send rr_transfer_snapshot finished!!");
    log_error("replic procedure finish! sendByes %d", sendBytes);

    return 0;
}

void ReplicationWorker::reportError(ReplicationJob *job) {
    send_error_to_redis(job->upstreamRedis);
    SSDBServer *serv = job->serv;

    {
        Locking<Mutex> l(&serv->replicMutex);
        serv->replicNumFailed++;
        if (serv->replicNumStarted == (serv->replicNumFinished + serv->replicNumFailed)) {
            serv->replicState = REPLIC_END;
        }
    }
    delete job->upstreamRedis;
    job->upstreamRedis = nullptr; //reset
}


void send_error_to_redis(Link *link) {
    if (link != nullptr) {
        link->quick_send({"error", "rr_transfer_snapshot unfinished"});
        log_error("send rr_transfer_snapshot error!!");
    }
}

std::string ssdb_save_len(uint64_t len) {
    std::string res;

    unsigned char buf[2];

    if (len < (1 << 6)) {
        /* Save a 6 bit len */
        buf[0] = (len & 0xFF) | (RDB_6BITLEN << 6);
        res.append(1, buf[0]);
    } else if (len < (1 << 14)) {
        /* Save a 14 bit len */
        buf[0] = ((len >> 8) & 0xFF) | (RDB_14BITLEN << 6);
        buf[1] = len & 0xFF;
        res.append(1, buf[0]);
        res.append(1, buf[1]);
    } else if (len <= UINT32_MAX) {
        /* Save a 32 bit len */
        buf[0] = RDB_32BITLEN;
        res.append(1, buf[0]);
        uint32_t len32 = htobe32(len);
        res.append((char *) &len32, sizeof(uint32_t));
    } else {
        /* Save a 64 bit len */
        buf[0] = RDB_64BITLEN;
        res.append(1, buf[0]);
        len = htobe64(len);
        res.append((char *) &len, sizeof(uint64_t));
    }
    return res;
}


void saveStrToBuffer(Buffer *buffer, const Bytes &fit) {
    string val_len = ssdb_save_len((uint64_t) (fit.size()));
    buffer->append(val_len);
    buffer->append(fit);
}

void moveBuffer(Buffer *dst, Buffer *src) {

    size_t comprlen, outlen = (size_t) src->size();
    void *out = zmalloc(outlen + 1);

    comprlen = lzf_compress(src->data(), (unsigned int) src->size(), out, outlen);

    dst->append(ssdb_save_len((uint64_t) src->size()));
    dst->append(ssdb_save_len(comprlen));
    dst->append(out, (int) comprlen);

    zfree(out);

    src->decr(src->size());
    src->nice();
}