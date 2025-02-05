/**************************************************************************\
|
|    Copyright (C) 2009 Marc Stevens
|
|    This program is free software: you can redistribute it and/or modify
|    it under the terms of the GNU General Public License as published by
|    the Free Software Foundation, either version 3 of the License, or
|    (at your option) any later version.
|
|    This program is distributed in the hope that it will be useful,
|    but WITHOUT ANY WARRANTY; without even the implied warranty of
|    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
|    GNU General Public License for more details.
|
|    You should have received a copy of the GNU General Public License
|    along with this program.  If not, see <http://www.gnu.org/licenses/>.
|
\**************************************************************************/

#ifndef MAIN_HPP
#define MAIN_HPP

#include <iostream>
#include <vector>
#include <string>
#include <thread>
#include <mutex>
#include <atomic>
#include <random>
#include <unordered_map>

#include <boost/filesystem/operations.hpp>

#include <hashclash/sdr.hpp>
#include <hashclash/saveload_gz.hpp>
#include <hashclash/differentialpath.hpp>
#include <hashclash/rng.hpp>
#include <hashclash/md5detail.hpp>

using namespace hashclash;
using namespace std;

extern std::string workdir;

unsigned load_block(istream &i, uint32 block[]);
void save_block(ostream &o, uint32 block[]);

struct parameters_type {
    uint32 m_diff[16];
    string pathfile, prefixfile;
    uint32_t ihv[4];
    string alphabet;
    vector<string> byte_alphabet;

    int threads;

    void show_mdiffs() {
        for (unsigned k = 0; k < 16; ++k) {
            if (m_diff[k] != 0) {
                cout << "delta_m[" << k << "] = " << naf(m_diff[k]) << endl;
            }
        }
    }
};

int textcoll_block1(parameters_type &parameters);
int textcoll_block2(parameters_type &parameters);

static inline size_t hammingweight(uint32_t x) { return __builtin_popcountl(x); }
static inline size_t hammingweight(uint64_t x) { return __builtin_popcountll(x); }

struct counter_exponential_print {
    counter_exponential_print(const std::string &_name = "counter", uint64_t _c = 0)
        : name(_name), c(_c) {}
    const uint64_t operator()() const { return c; }
    uint64_t operator++() {
        if (hammingweight(++c) == 1) {
            std::cout << " " << name << "=" << c << std::flush;
        }
        return c;
    }
    std::atomic_uint64_t c;
    std::string name;
};

inline static std::vector<std::pair<size_t, size_t>> split_workload(size_t N, size_t jobs) {
    std::vector<std::pair<size_t, size_t>> ret(jobs);
    if (jobs == 0) {
        return ret;
    }
    size_t frac = N / jobs, rem = N % jobs;
    for (size_t i = 0; i < jobs; ++i) {
        if (i == 0) {
            ret[i].first = 0;
        } else {
            ret[i].first = ret[i - 1].second;
        }
        ret[i].second = ret[i].first + frac + (i < rem ? 1 : 0);
    }
    if (ret.back().second != N) {
        throw std::runtime_error("split_wordload-inconsistency");
    }
    return ret;
}

template <typename F> void run_workload(size_t jobs, F &&f) {
    std::vector<std::thread> threadgroup;
    for (size_t i = 0; i < jobs; ++i) {
        threadgroup.emplace_back(std::forward<F>(f), i, jobs);
    }
    for (size_t i = 0; i < jobs; ++i) {
        threadgroup[i].join();
    }
}

template <typename T> void randomize_vector(std::vector<T> &in) {
    for (size_t i = 0; i < in.size(); ++i) {
        size_t j = xrng64() % in.size();
        if (i == j) {
            continue;
        }
        std::swap(in[i], in[j]);
    }
}

struct localxrng {
    uint64_t x, y;

    localxrng() {
        std::random_device rd;
        x = uint64_t(rd()) + (uint64_t(rd()) << 32);
        y = uint64_t(rd()) + (uint64_t(rd()) << 32);
        x |= 1;
    }

    uint32_t operator()() {
        uint64_t t = x, s = y;
        x = y;
        t ^= t << 23;
        t ^= t >> 18;
        t ^= s ^ (s >> 5);
        y = t;
        return uint32_t((t + s) >> 32);
    }
};

template <typename UInt = size_t> UInt Binomial(UInt N, UInt K) {
    if (K == 0 || K >= N) {
        return 1;
    }
    if (K > N / 2) {
        return Binomial<UInt>(N, N - K);
    }
    return Binomial<UInt>(N - 1, K - 1) * N / K;
}

// range-expression for bit mask:
//    input: UInt mask
//    let S = { UInt x | (x & ~mask) == 0 }
//    then |S| = 2^hammingweight(mask)
//    range-expression: enumerate x in S in lexicographical order
// usage:
//    for (auto&& x : bit_mask_range(mask)) { loop-statement; }
template <typename UInt> class bit_mask_range_t {
    UInt _mask;

  public:
    explicit bit_mask_range_t(UInt mask)
        : _mask(mask) {}
    size_t count(UInt mask) const { return size_t(1) << hammingweight(mask); }

    struct iterator {
        UInt _cur, _mask;
        iterator(UInt mask = 0)
            : _cur(0), _mask(mask) {}
        UInt operator*() const { return _cur; }
        // only implement correct comparison between non-end (_mask != 0) and end (_mask = 0)
        bool operator!=(const iterator &other) const { return _mask != other._mask; }
        iterator &operator++() {
            // compute next value in lexicographical order
            _cur = ((_cur | ~_mask) + 1) & _mask;
            // on wrap-around to 0 set _mask to 0 to indicate end
            if (_cur == 0) {
                _mask = 0;
            }
            return *this;
        }
    };
    iterator begin() const { return iterator(_mask); }
    iterator end() const { return iterator(); }
};
template <typename UInt = uint32_t> bit_mask_range_t<UInt> bit_mask_range(UInt mask) { return bit_mask_range_t<UInt>(mask); }

// range-expression for bit mask with minimum weight
//    input: UInt mask, unsigned minweight
//    let S = { UInt x | (x & ~mask) == 0 && hammingweight(x) >= minweight }
//    then |S| = sum_{k=minweight}^{hammingweight(mask)} Binomial(hammingweight(mask), k)
//    range-expression: enumerate x in S in lexicographical order
// usage:
//    for (auto&& x : bit_mask_minweight_range(mask, minweight)) { loop-statement; }

template <typename UInt> class bit_mask_minweight_range_t {
    UInt _mask;
    unsigned _minweight;
    UInt _LSBs[sizeof(UInt) * 8];

  public:
    explicit bit_mask_minweight_range_t(UInt mask, unsigned minweight)
        : _mask(mask), _minweight(minweight) {
        if (hammingweight(mask) < minweight) {
            _mask = _minweight = 0;
        }
        UInt LSB = 0;
        for (unsigned k = 0;; ++k) {
            LSB |= UInt(1) << k;
            LSB &= _mask;
            _LSBs[hammingweight(LSB)] = LSB;
            if (LSB == _mask) {
                break;
            }
        }
    }
    size_t count() const {
        size_t c = 0, n = hammingweight(_mask);
        for (unsigned k = _minweight; k <= n; ++k) {
            c += Binomial<size_t>(n, k);
        }
        return c;
    }

    struct iterator {
        const bit_mask_minweight_range_t *_parent;
        UInt _cur, _mask;

        iterator(const bit_mask_minweight_range_t<UInt> *parent = nullptr, UInt cur = 0, UInt mask = 0)
            : _parent(parent), _cur(cur), _mask(mask) {}

        UInt operator*() const { return _cur; }
        // only implement correct comparison between non-end (_mask != 0) and end (_mask = 0)
        bool operator!=(const iterator &other) const { return _mask != other._mask; }
        iterator &operator++() {
            // compute next value in lexicographical order
            _cur = ((_cur | ~_mask) + 1) & _mask;
            // on wrap-around to 0 set _mask to 0 to indicate end
            if (_cur == 0) {
                _mask = 0;
            } else if (hammingweight(_cur) < _parent->_minweight) {
                // set the least amount of LSBs to 1 to reach minweight
                for (unsigned k = _parent->_minweight - hammingweight(_cur); hammingweight(_cur) < _parent->_minweight; ++k) {
                    _cur |= _parent->_LSBs[k];
                }
            }
            return *this;
        }
    };
    iterator begin() const { return iterator(this, _LSBs[_minweight], _mask); }
    iterator end() const { return iterator(); }
};
template <typename UInt = uint32_t> bit_mask_minweight_range_t<UInt> bit_mask_minweight_range(UInt mask, unsigned minweight) {
    return bit_mask_minweight_range_t<UInt>(mask, minweight);
}

inline static void printword(uint32_t mt) {
    for (unsigned b = 0; b < 4; ++b) {
        std::cout << char((mt >> (8 * b)) & 0xFF);
    }
}
inline static std::string word2str(uint32_t mt) {
    std::string ret("0123");
    for (unsigned b = 0; b < 4; ++b) {
        ret[b] = char((mt >> (8 * b)) & 0xFF);
    }
    return ret;
}

struct byte_alphabet {
    // Bitmap that indicates if a particular character is present.
    uint64_t byte_ok[4];

    // Total number of characters in the alphabet.
    uint64_t byte_size;

    // The list of present characters.
    uint8_t byte_val[256];

    byte_alphabet()
        : byte_size(0), byte_ok{0, 0, 0, 0} {}

    byte_alphabet(const std::string &alphabet)
        : byte_size(0), byte_ok{0, 0, 0, 0} {
        // Dedupe and sort the characters of the alphabet.
        for (size_t byte : alphabet) {
            byte_ok[byte / 64] |= uint64_t(1) << (byte % 64);
        }
        for (unsigned i = 0; i < 256; ++i) {
            if (!check(uint8_t(i))) {
                continue;
            }
            byte_val[byte_size] = uint8_t(i);
            ++byte_size;
        }
    }

    uint64_t size() const { return byte_size; }

    bool check(uint8_t byte) const { return 1ULL == (1ULL & (byte_ok[byte / 64] >> (byte % 64))); }

    const uint8_t *begin() const { return byte_val + 0; }
    const uint8_t *end() const { return byte_val + byte_size; }
};

struct message_alphabet {
    byte_alphabet bytes[64];
    uint64_t word_size[16];

    static uint64_t wordsize(const byte_alphabet *bytes) {
        if (bytes == nullptr) { return 0; }
        uint64_t size = 1;
        for (unsigned b = 0; b < 4; b++) {
            size *= bytes[b].size();
        }
        return size;
    }

    message_alphabet(const std::string &alphabet = "", const std::vector<std::string> &byte_specific = std::vector<std::string>()) {
        for (unsigned b = 0; b < 64; ++b) {
            bytes[b] = byte_alphabet(alphabet);
            if (b < byte_specific.size() && !byte_specific[b].empty()) {
                bytes[b] = byte_alphabet(byte_specific[b]);
            }
        }
        for (unsigned w = 0; w < 16; ++w) {
            word_size[w] = wordsize(bytes + w*4);
        }
    }

    void set_byte_alphabet(int b, const std::string &alphabet) {
        bytes[b] = byte_alphabet(alphabet);
        // update corresponding word_size
        unsigned w = b / 4;
        word_size[w] = 1;
        for (unsigned i = 0; i < 4; ++i) {
            word_size[w] *= bytes[w * 4 + i].size();
        }
    }

    static bool checkword(const byte_alphabet *bytes, uint32_t w) {
        for (unsigned b = 0; b < 4; ++b) {
            uint8_t v = uint8_t((w >> (b * 8)) & 0xFF);
            if (!bytes[b].check(v)) {
                return false;
            }
        }
        return true;
    }

    bool checkword(int t, uint32_t w) const {
        return checkword(bytes + 4*t, w);
    }

    // Returns the Nth valid word of the given byte alphabets.
    static uint32_t makeword(const byte_alphabet *bytes, uint32_t n) {
        uint32_t w = 0;
        for (auto word_byte = 0; word_byte < 4; word_byte++) {
            auto byte_alpha = bytes[word_byte];
            auto alpha_size = byte_alpha.size();
            auto alpha_num = n % alpha_size;
            n = n / alpha_size;
            w |= uint32_t(byte_alpha.byte_val[alpha_num]) << (word_byte * 8);
        }
        return w;
    }

    uint32_t makeword(int word_num, uint32_t n) const {
        return makeword(bytes + 4*word_num, n);
    }

    uint32_t sampleword(int word_num) const {
        return makeword(word_num, xrng64());
    }

    uint32_t sampleword(int word_num, localxrng &rng) const {
        return makeword(word_num, rng());
    }

    struct word_range_t {
        const byte_alphabet *_bytes;

        explicit word_range_t(const byte_alphabet *bytes)
            : _bytes(bytes) {}

        uint64_t count() const {
            return wordsize(_bytes);
        }

        uint32_t operator[](uint32_t n) {
            return makeword(_bytes, n);
        }

        // Iterator over all messages that can be generated from the given alphabets.
        struct iterator {
            const byte_alphabet *_bytes;
            size_t _index[4];

            iterator(const byte_alphabet *bytes = nullptr)
                : _bytes(bytes), _index{0, 0, 0, 0} {}
            bool operator!=(const iterator &other) const { return _bytes != other._bytes; }
            iterator &operator++() {
                int b = 0;
                while (b < 4 && ++_index[b] >= _bytes[b].size()) {
                    _index[b] = 0;
                    ++b;
                }
                if (b == 4) {
                    _bytes = nullptr;
                }
                return *this;
            }
            uint32 operator*() const {
                uint32 w = 0;
                w += uint32(_bytes[0].byte_val[_index[0]]) << 0;
                w += uint32(_bytes[1].byte_val[_index[1]]) << 8;
                w += uint32(_bytes[2].byte_val[_index[2]]) << 16;
                w += uint32(_bytes[3].byte_val[_index[3]]) << 24;
                return w;
            }
        };

        iterator begin() const { return iterator(_bytes); }
        iterator end() const { return iterator(); }
    };

    word_range_t word_range(int t) const { return word_range_t(bytes + (4 * t)); }

    // Iterates over all valid word values in a pseudo-random order using an LCG.
    struct shuffled_word_range_t {
        const byte_alphabet *_bytes;
        uint32_t _seed;

        explicit shuffled_word_range_t(const byte_alphabet *bytes, uint32_t seed)
            : _bytes(bytes), _seed(seed) {}

        uint64_t count() const {
            return wordsize(_bytes);
        }

        struct lcg_iterator {
            const byte_alphabet *_bytes;
            uint32_t state, step, value, value_mask, max_value;

            // Steele, Guy & Vigna, Sebastiano. (2021). "Computationally easy,
            // spectrally good multipliers for congruential pseudorandom number generators."
            // Software: Practice and Experience. 52. 10.1002/spe.3030.
            void lcg() {
                // We can't just use (state % max_value) otherwise the LCG isn't guaranteed
                // to have a full period according to the Hull-Dobell theorem.
                // Instead, we end up rejecting at most half of generated LCG values.
                do {
                    state = (state * 0x915f77f5) + 1;
                    value = (state & value_mask);
                } while (value >= max_value);
            }

            bool done() const { return (_bytes == nullptr || step >= max_value); }

            explicit lcg_iterator(const byte_alphabet *bytes = nullptr, uint32_t seed = 0)
                : _bytes(bytes), state(seed), step(0), value(0), max_value(wordsize(_bytes)) {
                value_mask = ((uint64_t)1 << (32 - __builtin_clzl(max_value | 1))) - 1;
                if (_bytes != nullptr) { lcg(); }
            }

            explicit lcg_iterator(const byte_alphabet *bytes)
                : lcg_iterator(bytes, xrng32()) {}

            bool operator==(const lcg_iterator &other) const {
                if (_bytes == nullptr && other._bytes == nullptr) { return true; }
                return (
                    _bytes == other._bytes
                    && state == other.state
                    && step == other.step
                );
            }
            bool operator!=(const lcg_iterator &other) const {
                return !(this->operator==(other));
            }

            lcg_iterator &operator++() {
                if (_bytes != nullptr) { lcg(); }
                step += 1;
                if (step >= max_value) { _bytes = nullptr; }
                return *this;
            }

            uint32 operator*() const {
                return makeword(_bytes, value);
            }
        };

        lcg_iterator begin() const { return lcg_iterator(_bytes, _seed); }
        lcg_iterator end() const { return lcg_iterator(); }
    };

    shuffled_word_range_t shuffled_word_range(int t, uint32_t seed) const {
        return shuffled_word_range_t(bytes + (4 * t), seed);
    }

    shuffled_word_range_t shuffled_word_range(int t) const {
        return shuffled_word_range(t, xrng64());
    }
};

struct masked_value {
    // For each 1-bit in the mask, the result must have the corresponding bit from the value.
    //   for (bit = 0..31) { result[bit] = (mask[bit] ? value[bit] : input[bit]); }
    // Or simply: output = (input & ~mask) | (value & mask)
    uint32_t mask;
    uint32_t value;

    masked_value(uint32_t m = 0, uint32_t v = 0)
        : mask(m), value(v & m) {}

    // Same as `(c & ~mask) == value`
    bool check(uint32_t c) const { return 0 == ((c ^ value) & mask); }

    // Generate a random value that has the required bit values.
    uint32_t sample() const { return (uint32_t(xrng64()) & ~mask) ^ value; }
    uint32_t sample(localxrng &rng) const { return (uint32_t(rng()) & ~mask) ^ value; }

    // The number of possible output values that can be generated with this mask.
    // e.g. If the mask has 8 bits set, we can generate 2^(32-8) results.
    uint64_t count() const { return uint64_t(1) << hammingweight(~mask); }

    // Iterates all possible word values that satisfy the given mask and value.
    struct range_t {
        uint32_t _loopvalue;
        uint32_t _loopmask;
        range_t(uint32_t loopvalue = 0, uint32_t loopmask = 0)
            : _loopvalue(loopvalue), _loopmask(loopmask) {}
        struct iterator {
            bit_mask_range_t<uint32_t>::iterator _iterator;
            uint32_t _loopvalue;
            iterator(uint32_t loopvalue = 0, uint32_t loopmask = 0)
                : _iterator(loopmask), _loopvalue(loopvalue) {}
            bool operator!=(const iterator &other) const { return _iterator != other._iterator; }
            iterator &operator++() {
                ++_iterator;
                return *this;
            }
            uint32_t operator*() const { return *_iterator ^ _loopvalue; }
        };
        iterator begin() const { return iterator(_loopvalue, _loopmask); }
        iterator end() const { return iterator(); }
    };
    range_t range() const { return range_t(value, ~mask); }
    range_t range_rnd() const { return range_t(value ^ (xrng64() & ~mask), ~mask); }
    range_t range_rnd(localxrng &rng) const { return range_t(value ^ (rng() & ~mask), ~mask); }
};

template <size_t N> struct md5state_t {
    static const int offset = 3;
    uint32 Q[N];
    uint32 m[16];

    md5state_t() = default;
    md5state_t(const md5state_t<N> &) = default;

    template <size_t N2> md5state_t(const md5state_t<N2> &s) {
        for (unsigned i = 0; i < 16; ++i) {
            m[i] = s.m[i];
        }
        for (unsigned i = 0; i < N && i < N2; ++i) {
            Q[i] = s.Q[i];
        }
    }

    uint32 &Qt(int t) { return Q[offset + t]; }
    const uint32 &Qt(int t) const { return Q[offset + t]; }
    uint32 &Wt(int t) { return m[md5_wt[t]]; }
    const uint32 &Wt(int t) const { return m[md5_wt[t]]; }

    // Compute Qt+1 using Qt, Qt-1, Qt-2, Qt-3, and Wt.
    // This is the normal "forward" processing the MD5 does.
    void computeQtp1(int t) { Qt(t + 1) = md5_step(t, Qt(t), Qt(t - 1), Qt(t - 2), Qt(t - 3), Wt(t)); }

    // Compute Qt-3 using Qt+1, Qt, Qt-1, Qt-2, and Wt.
    void computeQtm3(int t) { Qt(t - 3) = md5_step_bw(t, Qt(t + 1), Qt(t), Qt(t - 1), Qt(t - 2), Wt(t)); }

    // Compute Wt using Qt+1, Qt, Qt-1, Qt-2, Qt-3.
    void computeWt(int t) { Wt(t) = md5_step_bw(t, Qt(t + 1), Qt(t), Qt(t - 1), Qt(t - 2), Qt(t - 3)); }
};

#ifndef NOSERIALIZATION
namespace boost {
namespace serialization {

template <class Archive, size_t N> void serialize(Archive &ar, ::md5state_t<N> &p, const unsigned int file_version) {
    size_t n = N;
    ar &make_nvp("N", n);
    if (n != N) {
        throw std::runtime_error("md5state_t: N mismatch in loading");
    }
    for (size_t i = 0; i < 16; ++i) {
        ar &make_nvp("m", p.m[i]);
    }
    for (size_t i = 0; i < N; ++i) {
        ar &make_nvp("Q", p.Q[i]);
    }
}

} // namespace serialization
} // namespace boost
#endif // NOSERIALIZATION

struct textcoll_solver_t {
    textcoll_solver_t()
        : testcounts(1 << 20, 0), threads(40) {}

    uint64 cpu_step_t[64];
    vector<uint64> testcounts;
    differentialpath diffpath; // Unused?
    uint32 m_diff[16];
    unsigned threads;

    static const int offset = 3;

    // The arithmetic difference in Q, T, and R after applying the message difference.
    uint32 dQ[68];
    uint32 dT[68];
    uint32 dR[68];

    uint32 dQt(int t) const { return dQ[offset + t]; }
    uint32 dTt(int t) const { return dT[offset + t]; }
    uint32 dRt(int t) const { return dR[offset + t]; }

    // The bits of Qt that must have a set value or depend on previous Q value.
    // i.e. If a bit in this mask is 1, the value is not ours to choose.
    uint32 Qvaluemask[68];
    uint32 Qtvaluemask(int t) const { return Qvaluemask[offset + t]; }

    // The bits of Qt that must be set to 1 or must be different from Qt-1 or Qt-2 (???).
    uint32 Qvalue[68];
    uint32 Qtvalue(int t) const { return Qvalue[offset + t]; }

    // The bits of Qt that depend on the value of Qt-1
    uint32 Qprev[68];
    uint32 Qtprev(int t) const { return Qprev[offset + t]; }

    // The bits of Qt that depend on the value of Qt-2
    // TODO: this appears to be unused
    uint32 Qprev2[68];
    uint32 Qtprev2(int t) const { return Qprev2[offset + t]; }

    size_t prefixblocks;
    uint32 ihv1[4];
    uint32 ihv2[4];


    message_alphabet MA;

    void filltables(const differentialpath &_diffpath);
    void fillalphabet(const std::string &alphabet, const std::vector<std::string> &byte_specific = std::vector<std::string>()) {
        MA = message_alphabet(alphabet, byte_specific);
    }

    typedef md5state_t<32> halfstate_t;
    typedef md5state_t<68> fullstate_t;
    typedef vector<halfstate_t> vec_halfstate_t;

    void prepare_block1();

    void generateQ13Q17(vec_halfstate_t &out, uint64_t N);
    void extend_step_fw(int t, vec_halfstate_t &out, const vec_halfstate_t &in, uint64_t N);
    void extend_step_bw(int t, vec_halfstate_t &out, const vec_halfstate_t &in, uint64_t N);
    void extend_step_m11(vec_halfstate_t &out, const vec_halfstate_t &in, uint64_t N);
    void extend_step_m10(vec_halfstate_t &out, const vec_halfstate_t &in, uint64_t N);

    void start_block1();

    void completeQ7Q24(const halfstate_t &Q7Q24state);
    void compute_good_m10(const fullstate_t &Sin);
    void check_solution(const fullstate_t &sol);

    std::vector<uint32_t> m4rndrange, good_m10;
    std::vector<std::array<uint32_t, 4>> vecQ7m10m12m13;
    std::unordered_map<uint32_t, size_t> Q7ptr;

    void start_block2();

    std::mutex mut;
    typedef std::lock_guard<std::mutex> lock_t;

    // Returns a mask indicating the bits of Q9 that can be modified for the Q9m9 tunnel
    template <size_t N> uint32_t Q9m9tunnelmask(const md5state_t<N> &S, uint32_t Q10, uint32_t Q11) const {
        return (
            // Qvaluemask is the bits of Qt that must be a set value according to the differential path.
            // ~Qtvaluemask(9) are the bits of Q9 that we are allowed to manipulate.
            ~Qtvaluemask(9)
            // Qprev are the bits of Qt that depend on values from the previous step.
            // ~Qtprev(10) are the bits of Q10 that don't depend on values from Q9.
            & ~Qtprev(10)
            // As required by tunnel T8 (Section 6.3.1 table 6-3),
            // the extra bit conditions that Q10[b] = 0 and Q11[b] = 1
            & ~Q10
            & Q11
        );
    }

    template <size_t N> uint32_t Q9m9tunnelmask(const md5state_t<N> &S) const {
        return Q9m9tunnelmask(S, S.Qt(10), S.Qt(11));
    }

    // Returns the Q9m9 tunnel strength (between 0 and 31 bits of freedom)
    template <size_t N> unsigned Q9m9tunnel(const md5state_t<N> &S, uint32_t Q10, uint32_t Q11) const {
        return hammingweight(Q9m9tunnelmask(S, Q10, Q11));
    }

    template <size_t N> unsigned Q9m9tunnel(const md5state_t<N> &S) const {
        return hammingweight(Q9m9tunnelmask(S));
    }

    // Ensure that the message difference carry/borrow propagates correctly for this Q value
    // because the step's rotation may disrupt carry/borrow propagation.
    // e.g. 0xFFFFFFFF + 1 will carry propagate across the 32 bits,
    //   but rotate_left(0xFFFFFFFF, X) + rotate_left(1, X) only propagates across 32-X bits.
    // How far the message difference propagates depends on both Q and the amount of rotation.
    // See 5.5.1 Rotation of word differences
    template <size_t N> bool checkrotationQtQtp1(int t, const md5state_t<N> &s) const {
        uint32_t R1 = s.Qt(t + 1) - s.Qt(t);
        uint32_t R2 = R1 + dRt(t);
        uint32_t T1 = rotate_right(R1, md5_rc[t]);
        uint32_t T2 = rotate_right(R2, md5_rc[t]);
        return T2 - T1 == dTt(t);
    }

    // The directly required bits of Qt in isolation (no previous or next dependencies).
    template <size_t N> masked_value masked_value_Qt(int t, const md5state_t<N> &s) const {
        // ignore Qprev(t) (included by default) and Qprev(t+1) (not included by default)
        uint32_t mask = Qtvaluemask(t) & ~Qtprev(t);
        return masked_value(mask, Qtvalue(t) & mask);
    }

    // The required bits of Qt based on the current value of Qt-1.
    // Assumes that Qt+1 is unknown or unsolved.
    template <size_t N> masked_value masked_value_QtQtm1(int t, const md5state_t<N> &s) const {
        // use Qprev(t) included by default
        return masked_value(Qtvaluemask(t), Qtvalue(t) ^ (s.Qt(t - 1) & Qtprev(t)));
    }

    // The required bits of Qt based on the current value of Qt+1.
    // Assumes that Qt-1 is unknown or unspecified.
    template <size_t N> masked_value masked_value_QtQtp1(int t, const md5state_t<N> &s) const {
        // ignore Qprev(t) included by default
        uint32_t mask = Qtvaluemask(t) & ~Qtprev(t);
        uint32_t value = Qtvalue(t) & mask;
        // use Qprev(t+1) not included by default
        mask |= Qtprev(t + 1);
        value ^= (s.Qt(t + 1) ^ Qtvalue(t + 1)) & Qtprev(t + 1);
        return masked_value(mask, value);
    }

    template <size_t N> bool checkQt(int t, const md5state_t<N> &s) const { return masked_value_Qt(t, s).check(s.Qt(t)); }
    template <size_t N> bool checkQtQtm1(int t, const md5state_t<N> &s) const { return masked_value_QtQtm1(t, s).check(s.Qt(t)); }
    template <size_t N> bool checkQtQtp1(int t, const md5state_t<N> &s) const { return masked_value_QtQtp1(t, s).check(s.Qt(t)); }
};

#endif // MAIN_HPP
