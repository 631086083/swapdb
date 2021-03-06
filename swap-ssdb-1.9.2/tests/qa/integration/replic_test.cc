//
// Created by a1 on 17-1-13.
//
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <vector>
#include <sstream>
#include "SSDB_client.h"
#include "gtest/gtest.h"
#include "ssdb_test.h"
#include <time.h>
#include <util/strings.h>

using namespace std;
#undef NDEBUG
class SlaveClient{
public:
    SlaveClient(string ip, int port){
        slaveclient = ssdb::Client::connect(ip.data(), port);
        assert(NULL != slaveclient);
    }

    virtual ~SlaveClient(){
        delete slaveclient;
    }

    ssdb::Client* client(){
        return slaveclient;
    }

private:
    ssdb::Client *slaveclient;
};

class ReplicTest : public SSDBTest {
public:
    ReplicTest(){
        keysNum = 100;
        val = "val";
        field = "field";
        score = 0;
        counts = 100;
        slave_ip = "127.0.0.1";
        slave_port = 8889;
    }

    SlaveClient* sclient;
    ssdb::Status s;
    string key, val, getVal, field;
    std::vector<std::string> keys, getList;
    uint32_t keysNum, counts;
    int64_t ret;
    double score, getScore;
    static const int threadsNum = 2;
    pthread_t bg_tid[threadsNum];
    void * status;
    static void* replic_thread_func(void *arg);

    void fillMasterData();
    void checkSlaveDataOK(int);
    void checkSlaveDataNotFound();
    void replic(const string &slaveip, int slaveport);
    void replic(const std::vector<std::string> &items);

protected:
    string slave_ip;
    int slave_port;
};

void* ReplicTest::replic_thread_func(void *arg) {

    SlaveClient* replicClient = new SlaveClient("127.0.0.1", 8888);
    replicClient->client()->replic("127.0.0.1", *((int*)arg));

    // std::string result = replicClient->client()->response();
    replicClient->client()->del_snapshot();
    delete replicClient;
}

void ReplicTest::fillMasterData(){
    keys.clear();
    for (int n = 0; n < counts; ++n) {
        //string type
        key = "kkey_"+itoa(n);
        for (int m = 0; m < keysNum; ++m) {
            client->set(key + "_" +itoa(m), val+itoa(m));
            keys.push_back(key+ "_"+itoa(m));
        }

        //hash type
        key = "hkey_"+itoa(n);
        for (int m = 0; m < keysNum; ++m) {
            client->hset(key, field+itoa(m), val+itoa(m));
        }
        keys.push_back(key);

        //list type
        key = "lkey_"+itoa(n);
        for (int m = 0; m < keysNum; ++m) {
            client->qpush_back(key, val+itoa(m), &ret);
        }
        keys.push_back(key);

        //set type
        key = "skey_"+itoa(n);
        for (int m = 0; m < keysNum; ++m) {
            client->sadd(key, val+itoa(m), &ret);
        }
        keys.push_back(key);

        //zset type
        key = "zkey_"+itoa(n);
        for (int m = 0; m < keysNum; ++m) {
            client->zset(key, field+itoa(m), score+m);
        }
        keys.push_back(key);
    }
}

void ReplicTest::checkSlaveDataOK(int times=10) {
    for (int t = 0; t < times; t++) { s = sclient->client()->get("kkey_0_0", &getVal);
        if (s.ok())
            break;
        else sleep(1);
    }

    ASSERT_TRUE(s.ok())<<"replic not finish in "<<times<<" secs."<<s.code()<<endl;

    for (int n = 0; n < counts; ++n) {
        //string type
        key = "kkey_"+itoa(n);
        for (int m = 0; m < keysNum; ++m) {
            sclient->client()->get(key+ "_"+itoa(m), &getVal);
            ASSERT_EQ(val+itoa(m), getVal)<<key+"_"+itoa(m);
        }

        //hash type
        key = "hkey_"+itoa(n);
        sclient->client()->hsize(key, &ret);
        ASSERT_EQ(keysNum, ret)<<"slave hsize error"<<endl;

        for (int m = 0; m < keysNum; ++m) {
            sclient->client()->hget(key, field+itoa(m), &getVal);
            ASSERT_EQ(val+itoa(m), getVal)<<key<<":"<<m;
        }

        //list type
        key = "lkey_"+itoa(n);
        sclient->client()->qsize(key, &ret);
        ASSERT_EQ(keysNum, ret)<<"slave qsize error"<<endl;

        for (int m = 0; m < keysNum; ++m) {
            sclient->client()->qget(key, m, &getVal);
            ASSERT_EQ(val+itoa(m), getVal)<<key<<":"<<m;
        }

        //set type
        key = "skey_"+itoa(n);
        sclient->client()->scard(key, &ret);
        ASSERT_EQ(keysNum, ret)<<"slave scard error"<<endl;

        for (int m = 0; m < keysNum; ++m) {
            sclient->client()->sismember(key, val+itoa(m), &ret);
            ASSERT_EQ(true, ret)<<key<<":"<<m;
        }

        //zset type
        key = "zkey_"+itoa(n);
        sclient->client()->zsize(key, &ret);
        ASSERT_EQ(keysNum, ret)<<"slave zsize error"<<endl;

        for (int m = 0; m < keysNum; ++m) {
            sclient->client()->zget(key, field+itoa(m), &getScore);
            ASSERT_EQ(m, getScore)<<key<<":"<<m;
        }
    }
}

void ReplicTest::checkSlaveDataNotFound() {
    for (int n = 0; n < counts; ++n) {
        //string type
        key = "kkey_"+itoa(n);
        for (int m = 0; m < keysNum; ++m) {
            s = sclient->client()->get(key+ "_"+itoa(m), &getVal);
            ASSERT_TRUE(s.not_found())<<s.code();
        }

        //hash type
        key = "hkey_"+itoa(n);
        sclient->client()->hsize(key, &ret);
        ASSERT_EQ(0, ret)<<"slave hsize error"<<endl;

        //list type
        key = "lkey_"+itoa(n);
        sclient->client()->qsize(key, &ret);
        ASSERT_EQ(0, ret)<<"slave qsize error"<<endl;

        //set type
        key = "skey_"+itoa(n);
        sclient->client()->scard(key, &ret);
        ASSERT_EQ(0, ret)<<"slave scard error"<<endl;

        //zset type
        key = "zkey_"+itoa(n);
        sclient->client()->zsize(key, &ret);
        ASSERT_EQ(0, ret)<<"slave zsize error"<<endl;
    }
}

void ReplicTest::replic(const string &slave_ip, int slave_port) {
    SlaveClient* replicClient = new SlaveClient(ip, port);
    s = replicClient->client()->replic(slave_ip, slave_port);
    ASSERT_EQ("ok", s.code())<<"replic finished!"<<endl;

    // std::string result = replicClient->client()->response();
    replicClient->client()->del_snapshot();
    delete replicClient;
}

void ReplicTest::replic(const std::vector<std::string> &items) {
    SlaveClient* replicClient = new SlaveClient(ip, port);
    s = replicClient->client()->replic(items);

    std::string result = replicClient->client()->response();
    ASSERT_EQ("ok", result);
    delete replicClient;
}

TEST_F(ReplicTest, Test_replic_types) {
    fillMasterData();
    sclient = new SlaveClient(slave_ip, slave_port);

    replic(slave_ip, slave_port);

    checkSlaveDataOK();
    client->multi_del(keys);
    sclient->client()->multi_del(keys);
    delete sclient;
}

TEST_F(ReplicTest, Test_replic_lens) {
    // std::vector<int64_t> lens = {0, 1<<6-1, 1<<6, 1<<14-1, 1<<14, 1<<32-1, 1<<32};
    std::vector<int64_t> lens = {0, (1<<6)-1, 1<<6, (1<<14)-1, 1<<14};
    key = "key";
    sclient = new SlaveClient(slave_ip, slave_port);
    sclient->client()->multi_del(key);
    for (auto l : lens) {
        val.clear();
        val.append(l, 'a');
        client->set(key, val);

        replic(slave_ip, slave_port);

        for (int t = 0; t < 10; t++) {
            if (sclient->client()->get(key, &getVal).ok())
                break;
            else sleep(1);
        }

        EXPECT_EQ(l, getVal.size());
        sclient->client()->multi_del(key);
    }
    delete sclient;
    client->multi_del(key);
}

#ifdef EXPIRE
TEST_F(ReplicTest, Test_replic_expire_keys) {
    keysNum = 10;
    fillMasterData();
    sclient = new SlaveClient(slave_ip, slave_port);

    int16_t etime = 10;
    for (auto key : keys) {
        client->expire(key, etime);
    }
    time_t pre_seconds = time((time_t*)NULL);

    replic(slave_ip, slave_port);

    time_t post_seconds = time((time_t*)NULL);
    bool checkOK = false;
    while(post_seconds-pre_seconds<etime/2) {
        checkSlaveDataOK();
        sleep(1);
        post_seconds = time((time_t*)NULL);
        checkOK = true;
    }
    ASSERT_TRUE(checkOK)<<"should check ok at least once."<<endl;

    if(etime>post_seconds-pre_seconds)
        sleep(etime-(post_seconds-pre_seconds)+1);

    checkSlaveDataNotFound();
    delete sclient;
}
#endif

//For a replic twice issue
#ifdef EXPIRE
TEST_F(ReplicTest, Test_replic_expire_keys_override) {
    int64_t etime = 3;
    client->setx("key", "val", etime);
    replic(slave_ip, slave_port);
    client->set("key", "val");
    replic(slave_ip, slave_port);

    sclient = new SlaveClient(slave_ip, slave_port);

    sleep(etime);
    sclient->client()->ttl("key", &ret);
    ASSERT_EQ(-1, ret)<<"key should be a persist key."<<endl;

    client->multi_del("key");
    replic(slave_ip, slave_port);
    sclient->client()->ttl("key", &ret);
    ASSERT_EQ(-2, ret)<<"key should be deleted."<<endl;
    delete sclient;
}
#endif

TEST_F(ReplicTest, Test_replic_multi_slaves) {
    fillMasterData();

    std::vector<int> slave_ports = {8889, 8890};
    // ssdb not support replic two slaves at once now.
    // std::vector<std::string> items;
    // for (auto port : slave_ports) {
        // items.push_back(slave_ip);
        // items.push_back(itoa(port));
    // }
    // replic(items);
    for (auto port : slave_ports) {
        replic(slave_ip, port);
    }

    for (auto port : slave_ports) {
        sclient = new SlaveClient(slave_ip, port);
        checkSlaveDataOK();
        sclient->client()->multi_del(keys);
        delete sclient;
    }
    client->multi_del(keys);
}

TEST_F(ReplicTest, Test_replic_multi_slaves_mthreads) {
    fillMasterData();

    std::vector<int> slave_ports = {8889, 8890};
    for (int n = 0; n < threadsNum; n++) {
        pthread_create(&bg_tid[n], NULL, &replic_thread_func, &slave_ports[n]);
    }
    for(int n = 0; n < threadsNum; n++) {
        pthread_join(bg_tid[n], &status);
    }

    for (auto port : slave_ports) {
        sclient = new SlaveClient(slave_ip, port);
        checkSlaveDataOK();
        sclient->client()->multi_del(keys);
        delete sclient;
    }
    client->multi_del(keys);
}
