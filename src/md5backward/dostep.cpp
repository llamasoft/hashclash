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

#include <vector>
#include <algorithm>
#include <stdexcept>
#include <map>
#include <utility>
#include <algorithm>
#include <string>
#include <iostream>
#include <time.h>

#include <boost/lexical_cast.hpp>

#include <hashclash/saveload_gz.hpp>
#include <hashclash/md5detail.hpp>
#include <hashclash/rng.hpp>
#include <hashclash/differentialpath.hpp>
#include <hashclash/progress_display.hpp>
#include <hashclash/timer.hpp>

#include "main.hpp"

using namespace hashclash;
using namespace std;

void random_permutation(vector<differentialpath> &paths) {
    // use a pseudo-random permutation fixed by the number of paths
    seed(paths.size());
    for (unsigned i = 0; i < paths.size(); ++i) {
        unsigned k = xrng64() % paths.size();
        paths[i].swap(paths[k]);
    }
    addseed(time(NULL));
}

inline std::string pathsstring(const std::string &basepath, unsigned modi, unsigned modn) {
    return workdir + "/" + basepath + "_" + boost::lexical_cast<std::string>(modi) + "of" + boost::lexical_cast<std::string>(modn);
}

progress_display *dostep_progress = 0;
volatile unsigned dostep_index = 0;
struct dostep_thread {
    dostep_thread(vector<differentialpath> &in, path_container_autobalance &out)
        : pathsin(in), container(out) {}
    vector<differentialpath> &pathsin;
    path_container_autobalance &container;
    md5_backward_thread worker;
    void operator()() {
        try {
            while (true) {
                mut.lock();

                unsigned i = dostep_index;
                if (i >= pathsin.size()) {
                    mut.unlock();
                    break;
                }
                unsigned cnt = std::max<unsigned>(1, std::min<unsigned>(16, (pathsin.size() - i) / 128));
                unsigned iend = i + cnt;

                if (cnt != 0) {
                    (*dostep_progress) += cnt;
                }
                dostep_index = iend;
                mut.unlock();

                for (; i < iend; ++i) {
                    worker.md5_backward_differential_step(pathsin[i], container);
                }
            }
        } catch (std::exception &e) {
            cerr << "Worker thread: caught exception:" << endl << e.what() << endl;
        } catch (...) {
        }
    }
};
void dostep_threaded(vector<differentialpath> &in, path_container_autobalance &out) {
    dostep_index = 0;
    std::string tstring = "t=" + boost::lexical_cast<std::string>(out.t) + ": ";
    if (tstring.size() == 5) {
        tstring += " ";
    }
    if (out.estimatefactor) {
        dostep_progress = new progress_display(in.size(), true, cout, tstring, "      ", "e     ");
    } else {
        dostep_progress = new progress_display(in.size(), true, cout, tstring, "      ", "      ");
    }
    boost::thread_group mythreads;
    for (unsigned i = 0; i < out.threads; ++i) {
        mythreads.create_thread(dostep_thread(in, out));
    }
    mythreads.join_all();
    if (dostep_progress->expected_count() != dostep_progress->count()) {
        *dostep_progress += dostep_progress->expected_count() - dostep_progress->count();
    }
    delete dostep_progress;
}

vector<differentialpath> pathscache;
void dostep(path_container_autobalance &container, bool savetocache) {
    const unsigned t = container.t;
    const unsigned modn = container.modn;
    const unsigned modi = container.modi;

    cout << endl;
    cout << "==================== Step " << t << " ====================" << endl;

    vector<differentialpath> pathsin, pathstmp, pathsout;
    if (pathscache.size() != 0) {
        pathsin.swap(pathscache);
        random_permutation(pathsin);
    } else if (container.newinputpath) {
        differentialpath path;
        path.offset = -int(t) + 3;
        path.path.resize(4);
        pathsin.push_back(path);
        cout << "Generated 1 new path." << endl;
    } else if (container.inputfile.size() == 0) {
        for (unsigned k = 0; k < modn; ++k) {
            try {
                std::string filename = pathsstring("paths" + boost::lexical_cast<std::string>(t + 1), k, modn);
                hashclash::timer loadtime(true);
                cout << "Loading " << filename << "..." << flush;
                load_gz(pathstmp, filename, binary_archive);
                random_permutation(pathstmp);
                for (unsigned j = modi; j < pathstmp.size(); j += modn) {
                    pathsin.push_back(pathstmp[j]);
                }
                cout << "done: " << pathstmp.size() << " (work:" << pathsin.size() << "). (" << loadtime.time() << "s)" << endl;
            } catch (...) {
                cout << "failed." << endl;
            }
        }
    } else {
        bool failed = false;
        try {
            hashclash::timer loadtime(true);
            cout << "Loading " << container.inputfile << "..." << flush;
            load_gz(pathstmp, binary_archive, container.inputfile);
            random_permutation(pathstmp);
            for (unsigned j = modi; j < pathstmp.size(); j += modn) {
                pathsin.push_back(pathstmp[j]);
            }
            cout << "done: " << pathsin.size() << ". (" << loadtime.time() << "s)" << endl;
        } catch (...) {
            failed = true;
            cout << "failed." << endl;
        }
        if (failed) {
            try {
                hashclash::timer loadtime(true);
                cout << "Loading (text) " << container.inputfile << "..." << flush;
                load_gz(pathstmp, text_archive, container.inputfile);
                random_permutation(pathstmp);
                for (unsigned j = modi; j < pathstmp.size(); j += modn) {
                    pathsin.push_back(pathstmp[j]);
                }
                cout << "done: " << pathsin.size() << ". (" << loadtime.time() << "s)" << endl;
            } catch (...) {
                cout << "failed." << endl;
            }
        }
    }
    if (container.showinputpaths) {
        for (unsigned r = 0; r < pathsin.size(); ++r) {
            show_path(pathsin[r], container.m_diff);
            cout << endl;
        }
    }

    std::string tstring = "t=" + boost::lexical_cast<std::string>(t) + ": ";
    if (tstring.size() == 5) {
        tstring += " ";
    }

    if (container.estimatefactor != 0) {
        cout << "Estimating maxcond for upper bound " << unsigned(double(container.ubound) * container.estimatefactor)
             << " (=" << container.ubound << " * " << container.estimatefactor << ")..." << endl;
        dostep_threaded(pathsin, container);
        //		progress_display show_progress(pathsin.size(), true, cout, tstring, "      ", "e     ");
        //		for (unsigned k = 0; k < pathsin.size(); ++k,++show_progress)
        //			md5_backward_differential_step(pathsin[k], container);
        container.finish_estimate();
        cout << "Found maxcond = " << container.maxcond << endl;
    }

    dostep_threaded(pathsin, container);
    //	progress_display show_progress(pathsin.size(), true, cout, tstring, "      ", "      ");
    //	for (unsigned k = 0; k < pathsin.size(); ++k,++show_progress)
    //		md5_backward_differential_step(pathsin[k], container);

    pathstmp.swap(pathsout);
    container.export_results(pathsout);

    unsigned condcount = 0, mincond = container.pathsout.size() + 1;
    for (unsigned c = 0; c < container.pathsout.size(); c++) {
        if (!container.pathsout[c].size()) { continue; }
        condcount++;
        if (c < mincond) { mincond = c; }
    }
    cout << "Found " << condcount << " different condition counts:" << endl;
    for (unsigned c = 0; c < container.pathsout.size(); c++) {
        if (!container.pathsout[c].size()) { continue; }
        cout << "  count[" << c << "] = " << container.pathsout[c].size() << endl;
    }

    if (pathsout.size() > 0) {
        cout << "Current mincond = " << mincond << endl;
        show_path(pathsout[0], container.m_diff);
    } else {
        throw std::runtime_error("No valid differential paths found!");
    }
    std::string filenameout = pathsstring("paths" + boost::lexical_cast<std::string>(t), modi, modn);
    hashclash::timer savetime(true);
    cout << "Saving " << pathsout.size() << " paths..." << flush;
    if (savetocache) {
        pathsout.swap(pathscache);
    } else {
        save_gz(pathsout, filenameout, binary_archive);
    }
    cout << "done. (" << savetime.time() << "s)" << endl;
}
