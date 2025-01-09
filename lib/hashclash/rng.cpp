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

#include <time.h>
#include <iostream>
#include <memory>
#include <exception>
#include <random>
#include "rng.hpp"

namespace hashclash {
void getosrnd(uint32 buf[256]) {
    std::unique_ptr<std::random_device> rd;
    try {
        // Explicitly request /dev/urandom as a randomness source if available.
        // On Windows, this constructor argument is entirely ignored.
        rd = std::unique_ptr<std::random_device>(new std::random_device("/dev/urandom"));
    } catch (const std::exception &e) {
        // If not available, use the default randomness source instead.
        // Note, it may have finite entropy, but it's better than nothing.
        rd = std::unique_ptr<std::random_device>(new std::random_device());
    }
    // Resample the randomness to uint32 regardless of the source's default range.
    // This may result in multiple calls to the random device.
    std::uniform_int_distribution<uint32> rng;
    for (auto i = 0; i < 256; i++) {
        buf[i] = rng(*rd);
    }
}

uint32 seedd;
uint32 seed32_1;
uint32 seed32_2;
uint32 seed32_3;
uint32 seed32_4;

void seed(uint32 s) {
    seedd = 0;
    seed32_1 = s;
    seed32_2 = 2;
    seed32_3 = 3;
    seed32_4 = 4;
    for (unsigned i = 0; i < 0x1000; ++i) {
        xrng128();
    }
}

void seed(uint32 *sbuf, unsigned len) {
    seedd = 0;
    seed32_1 = 1;
    seed32_2 = 2;
    seed32_3 = 3;
    seed32_4 = 4;
    for (unsigned i = 0; i < len; ++i) {
        seed32_1 ^= sbuf[i];
        xrng128();
    }
    for (unsigned i = 0; i < 0x1000; ++i) {
        xrng128();
    }
}

void addseed(uint32 s) {
    xrng128();
    seed32_1 ^= s;
    xrng128();
}

void addseed(const uint32 *sbuf, unsigned len) {
    xrng128();
    for (unsigned i = 0; i < len; ++i) {
        seed32_1 ^= sbuf[i];
        xrng128();
    }
}

struct hashclash_rng__init {
    hashclash_rng__init() {
        seed(uint32(time(NULL)));
        uint32 rndbuf[256]; // uninitialized on purpose
        addseed(rndbuf, 256);
        getosrnd(rndbuf);
        addseed(rndbuf, 256);
    }
};
hashclash_rng__init hashclash_rng__init__now;

void hashclash_rng_hpp_init() { hashclash_rng__init here; }

} // namespace hashclash
