#include <signal.h>
#include <assert.h>

#include <algorithm>

#include <ep.hh>
#include <item.hh>

extern "C" {
    static rel_time_t basic_current_time(void) {
        return 0;
    }

    rel_time_t (*ep_current_time)() = basic_current_time;
}

class Counter : public HashTableVisitor {
public:

    int count;

    Counter(bool v) : count(), verify(v) {}

    void visit(StoredValue *v) {
        if (!v->isDeleted()) {
            count += 1;
            std::string key = v->getKey();
            value_t val = v->getValue();
            if (verify) {
                assert(key.compare(val->to_s()) == 0);
            }
        }
    }
private:
    bool verify;
};

static int count(HashTable &h, bool verify=true) {
    Counter c(verify);
    h.visit(c);
    return c.count;
}

static void store(HashTable &h, std::string &k) {
    Item i(k, 0, 0, k.c_str(), k.length());
    h.set(i);
}

static void storeMany(HashTable &h, std::vector<std::string> &keys) {
    std::vector<std::string>::iterator it;
    for (it = keys.begin(); it != keys.end(); it++) {
        std::string key = *it;
        store(h, key);
    }
}

static void addMany(HashTable &h, std::vector<std::string> &keys, bool expect) {
    std::vector<std::string>::iterator it;
    for (it = keys.begin(); it != keys.end(); it++) {
        std::string k = *it;
        Item i(k, 0, 0, k.c_str(), k.length());
        bool v = h.add(i);
        assert(expect == v);
    }
}

static std::vector<std::string> generateKeys(int num, int start=0) {
    std::vector<std::string> rv;

    for (int i = start; i < num; i++) {
        char buf[64];
        snprintf(buf, sizeof(buf), "key%d", i);
        std::string key(buf);
        rv.push_back(key);
    }

    return rv;
}

// ----------------------------------------------------------------------
// Actual tests below.
// ----------------------------------------------------------------------

static void testHashSize() {
    HashTable h;
    assert(count(h) == 0);

    std::string k = "testkey";
    store(h, k);

    assert(count(h) == 1);
}

static void testHashSizeTwo() {
    HashTable h;
    assert(count(h) == 0);

    std::vector<std::string> keys = generateKeys(5);
    storeMany(h, keys);
    assert(count(h) == 5);

    h.clear();
    assert(count(h) == 0);
}

static void testReverseDeletions() {
    alarm(10);
    HashTable h(5, 1);
    assert(count(h) == 0);
    const int nkeys = 10000;

    std::vector<std::string> keys = generateKeys(nkeys);
    storeMany(h, keys);
    assert(count(h) == nkeys);

    std::reverse(keys.begin(), keys.end());

    std::vector<std::string>::iterator it;
    for (it = keys.begin(); it != keys.end(); it++) {
        std::string key = *it;
        h.del(key);
    }

    assert(count(h) == 0);
}

static void testForwardDeletions() {
    alarm(10);
    HashTable h(5, 1);
    assert(h.getSize() == 5);
    assert(h.getNumLocks() == 1);
    assert(count(h) == 0);
    const int nkeys = 10000;

    std::vector<std::string> keys = generateKeys(nkeys);
    storeMany(h, keys);
    assert(count(h) == nkeys);

    std::vector<std::string>::iterator it;
    for (it = keys.begin(); it != keys.end(); it++) {
        std::string key = *it;
        h.del(key);
    }

    assert(count(h) == 0);
}

static void testFind(HashTable &h) {
    const int nkeys = 5000;

    std::vector<std::string> keys = generateKeys(nkeys);
    storeMany(h, keys);

    std::string missingKey = "aMissingKey";
    assert(h.find(missingKey) == NULL);

    std::vector<std::string>::iterator it;
    for (it = keys.begin(); it != keys.end(); it++) {
        std::string key = *it;
        assert(h.find(key));
    }
}

static void testFind() {
    HashTable h(5, 1);
    testFind(h);
}

static void testFindSmall() {
    HashTable h(5, 1, small);
    testFind(h);
}

static void testAdd() {
    HashTable h(5, 1);
    const int nkeys = 5000;

    std::vector<std::string> keys = generateKeys(nkeys);
    addMany(h, keys, true);

    std::string missingKey = "aMissingKey";
    assert(h.find(missingKey) == NULL);

    std::vector<std::string>::iterator it;
    for (it = keys.begin(); it != keys.end(); it++) {
        std::string key = *it;
        assert(h.find(key));
    }

    addMany(h, keys, false);
    for (it = keys.begin(); it != keys.end(); it++) {
        std::string key = *it;
        assert(h.find(key));
    }

    // Verify we can readd after a soft deletion.
    assert(h.softDelete(keys[0]));
    assert(!h.softDelete(keys[0]));
    assert(!h.find(keys[0]));
    assert(count(h) == nkeys - 1);


    Item i(keys[0], 0, 0, "newtest", 7);
    assert(h.add(i));
    assert(count(h, false) == nkeys);
}

static void testDepthCounting() {
    HashTable h(5, 1);
    const int nkeys = 5000;

    std::vector<std::string> keys = generateKeys(nkeys);
    storeMany(h, keys);

    HashTableDepthStatVisitor depthCounter;
    h.visitDepth(depthCounter);
    // std::cout << "Max depth:  " << depthCounter.maxDepth << std::endl;
    assert(depthCounter.max > 1000);
}

static void testPoisonKey() {
    std::string k("A\\NROBs_oc)$zqJ1C.9?XU}Vn^(LW\"`+K/4lykF[ue0{ram;fvId6h=p&Zb3T~SQ]82'ixDP");

    HashTable h(5, 1);

    store(h, k);
    assert(count(h) == 1);
}

int main() {
    testHashSize();
    testHashSizeTwo();
    testReverseDeletions();
    testForwardDeletions();
    testFind();
    testFindSmall();
    testAdd();
    testDepthCounting();
    testPoisonKey();
    exit(0);
}
