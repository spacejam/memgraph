#pragma once

#include <algorithm>
#include <chrono>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iostream>
#include <queue>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include "import/base_import.hpp"
#include "import/element_skeleton.hpp"
#include "import/fillings/array.hpp"
#include "import/fillings/bool.hpp"
#include "import/fillings/double.hpp"
#include "import/fillings/filler.hpp"
#include "import/fillings/float.hpp"
#include "import/fillings/from.hpp"
#include "import/fillings/id.hpp"
#include "import/fillings/int32.hpp"
#include "import/fillings/int64.hpp"
#include "import/fillings/label.hpp"
#include "import/fillings/skip.hpp"
#include "import/fillings/string.hpp"
#include "import/fillings/to.hpp"
#include "import/fillings/type.hpp"
#include "storage/model/properties/all.hpp"
#include "storage/model/properties/flags.hpp"
#include "storage/vertex_accessor.hpp"
#include "utils/command_line/arguments.hpp"
#include "utils/option.hpp"

using namespace std;

bool equal_str(const char *a, const char *b) { return strcasecmp(a, b) == 0; }

// CSV importer for importing multiple files regarding same graph.
// CSV format of file should be following:
//
class CSVImporter : public BaseImporter
{

public:
    using BaseImporter::BaseImporter;

    // Loads data from stream and returns number of loaded vertexes.
    size_t import_vertices(std::fstream &file)
    {
        return import(file, create_vertex, true);
    }

    // Loads data from stream and returns number of loaded edges.
    size_t import_edges(std::fstream &file)
    {
        return import(file, create_edge, false);
    }

private:
    // Loads data from file and returns number of loaded name.
    template <class F>
    size_t import(std::fstream &file, F f, bool vertex)
    {
        string line;
        vector<char *> sub_str;
        vector<unique_ptr<Filler>> fillers;
        vector<char *> tmp;

        // HEADERS
        if (!getline(file, line)) {
            err("No lines");
            return 0;
        }

        if (!split(line, parts_mark, sub_str)) {
            err("Illegal headers");
            return 0;
        }

        for (auto p : sub_str) {
            auto o = get_filler(p, tmp, vertex);
            if (o.is_present()) {
                fillers.push_back(o.take());
            } else {
                return 0;
            }
        }
        sub_str.clear();

        // LOAD DATA LINES
        size_t count = 0;
        size_t line_no = 1;
        ElementSkeleton es(db);
        while (std::getline(file, line)) {
            // if (line_no % 1000 == 0) {
            //     cout << line_no << endl;
            // }
            // cout << line << endl;
            sub_str.clear();
            es.clear();

            if (split(line, parts_mark, sub_str)) {
                check_for_part_count(sub_str.size() - fillers.size(), line_no);

                int n = min(sub_str.size(), fillers.size());
                for (int i = 0; i < n; i++) {
                    auto er = fillers[i]->fill(es, sub_str[i]);
                    if (er.is_present()) {
                        err(er.get(), " on line: ", line_no);
                    }
                }

                if (f(this, es, line_no)) {
                    count++;
                }
            }

            line_no++;
        }

        return count;
    }

    static bool create_vertex(CSVImporter *im, ElementSkeleton &es,
                              size_t line_no)
    {
        auto va = es.add_vertex();
        auto id = es.element_id();
        if (id.is_present()) {

            if (im->vertices.size() <= id.get()) {
                Option<Vertex::Accessor> empty =
                    make_option<Vertex::Accessor>();
                im->vertices.insert(im->vertices.end(),
                                    id.get() - im->vertices.size() + 1, empty);
            }
            if (im->vertices[id.get()].is_present()) {
                im->err("Vertex on line: ", line_no,
                        " has same id with another previously loaded vertex");
                return false;
            } else {
                im->vertices[id.get()] = make_option(std::move(va));
                return true;
            }
        } else {
            im->warn("Missing import local vertex id for vertex on "
                     "line: ",
                     line_no);
        }

        return true;
    }

    static bool create_edge(CSVImporter *im, ElementSkeleton &es,
                            size_t line_no)
    {
        auto o = es.add_edge();
        if (!o.is_present()) {
            return true;
        } else {
            im->err(o.get(), " on line: ", line_no);
            return false;
        }
    }

    // Returns filler for name:type in header_part. None if error occured.
    Option<unique_ptr<Filler>> get_filler(char *header_part,
                                          vector<char *> &tmp_vec, bool vertex)
    {
        tmp_vec.clear();
        split(header_part, type_mark, tmp_vec);
        if (tmp_vec.size() > 2) {
            err("To much sub parts in header part");
            return make_option<unique_ptr<Filler>>();
        } else if (tmp_vec.size() < 2) {
            if (tmp_vec.size() == 1) {
                warn(
                    "Column ", tmp_vec[0],
                    " doesn't have specified type so string type will be used");
                tmp_vec.push_back("string");
            } else {
                warn("Empty colum definition, skiping column.");
                std::unique_ptr<Filler> f(new SkipFiller());
                return make_option(std::move(f));
            }
        }

        const char *name = tmp_vec[0];
        const char *type = tmp_vec[1];

        // cout << name << " # " << type << endl;

        auto prop_key = [&](auto name, auto type) -> auto
        {
            if (vertex) {
                return db.vertex_property_key(name, Type(type));
            } else {
                return db.edge_property_key(name, Type(type));
            }
        };

        if (equal_str(type, "id")) {
            std::unique_ptr<Filler> f(
                name[0] == '\0'
                    ? new IdFiller()
                    : new IdFiller(make_option(prop_key(name, Flags::Int64))));
            return make_option(std::move(f));

        } else if (equal_str(type, "start_id") || equal_str(type, "from_id") ||
                   equal_str(type, "from") || equal_str(type, "source")) {
            std::unique_ptr<Filler> f(new FromFiller(*this));
            return make_option(std::move(f));

        } else if (equal_str(type, "label")) {
            std::unique_ptr<Filler> f(new LabelFiller(*this));
            return make_option(std::move(f));

        } else if (equal_str(type, "end_id") || equal_str(type, "to_id") ||
                   equal_str(type, "to") || equal_str(type, "target")) {
            std::unique_ptr<Filler> f(new ToFiller(*this));
            return make_option(std::move(f));

        } else if (equal_str(type, "type")) {
            std::unique_ptr<Filler> f(new TypeFiller(*this));
            return make_option(std::move(f));

        } else if (name[0] == '\0') { // OTHER FILLERS REQUIRE NAME
            warn("Unnamed column of type: ", type, " will be skipped.");
            std::unique_ptr<Filler> f(new SkipFiller());
            return make_option(std::move(f));

            // *********************** PROPERTIES
        } else if (equal_str(type, "bool")) {
            std::unique_ptr<Filler> f(
                new BoolFiller(prop_key(name, Flags::Bool)));
            return make_option(std::move(f));

        } else if (equal_str(type, "double")) {
            std::unique_ptr<Filler> f(
                new DoubleFiller(prop_key(name, Flags::Double)));
            return make_option(std::move(f));

        } else if (equal_str(type, "float")) {
            std::unique_ptr<Filler> f(
                new FloatFiller(prop_key(name, Flags::Float)));
            return make_option(std::move(f));

        } else if (equal_str(type, "int")) {
            std::unique_ptr<Filler> f(
                new Int32Filler(prop_key(name, Flags::Int32)));
            return make_option(std::move(f));

        } else if (equal_str(type, "long")) {
            std::unique_ptr<Filler> f(
                new Int64Filler(prop_key(name, Flags::Int64)));
            return make_option(std::move(f));

        } else if (equal_str(type, "string")) {
            std::unique_ptr<Filler> f(
                new StringFiller(prop_key(name, Flags::String)));
            return make_option(std::move(f));

        } else if (equal_str(type, "bool[]")) {
            std::unique_ptr<Filler> f(make_array_filler<bool, ArrayBool>(
                *this, prop_key(name, Flags::ArrayBool), to_bool));
            return make_option(std::move(f));

        } else if (equal_str(type, "float[]")) {
            std::unique_ptr<Filler> f(make_array_filler<float, ArrayFloat>(
                *this, prop_key(name, Flags::ArrayFloat), to_float));
            return make_option(std::move(f));

        } else if (equal_str(type, "double[]")) {
            std::unique_ptr<Filler> f(make_array_filler<double, ArrayDouble>(
                *this, prop_key(name, Flags::ArrayDouble), to_double));
            return make_option(std::move(f));

        } else if (equal_str(type, "int[]")) {
            std::unique_ptr<Filler> f(make_array_filler<int32_t, ArrayInt32>(
                *this, prop_key(name, Flags::ArrayInt32), to_int32));
            return make_option(std::move(f));

        } else if (equal_str(type, "long[]")) {
            std::unique_ptr<Filler> f(make_array_filler<int64_t, ArrayInt64>(
                *this, prop_key(name, Flags::ArrayInt64), to_int64));
            return make_option(std::move(f));

        } else if (equal_str(type, "string[]")) {
            std::unique_ptr<Filler> f(make_array_filler<string, ArrayString>(
                *this, prop_key(name, Flags::ArrayString), to_string));
            return make_option(std::move(f));

        } else {
            err("Unknown type: ", type);
            return make_option<unique_ptr<Filler>>();
        }
    }

    void check_for_part_count(long diff, long line_no)
    {
        if (diff != 0) {
            if (diff < 0) {
                // warn("Line no: ", line_no, " has less parts then "
                //                            "specified in header. Missing ",
                //      diff, " parts");
            } else {
                warn("Line no: ", line_no,
                     " has more parts then specified in header. Extra ", diff,
                     " parts");
            }
        }
    }
};

// Imports all -v "vertex_file_path.csv" vertices and -e "edge_file_path.csv"
// edges from specified files. Also defines arguments -d, -ad, -w, -err, -info.
// -d delimiter => sets delimiter for parsing .csv files. Default is ,
// -ad delimiter => sets delimiter for parsing arrays in .csv. Default is ,
// -w bool => turns on/off output of warnings. Default on.
// -err bool => turns on/off output of errors. Default on.
// -info bool => turns on/off output of info. Default on.
// Returns (no loaded vertices,no loaded edges)
std::pair<size_t, size_t>
import_csv_from_arguments(Db &db, std::vector<std::string> &para)
{
    DbAccessor t(db);
    CSVImporter imp(t, cerr);

    imp.parts_mark = get_argument(para, "-d", ",")[0];
    imp.parts_array_mark = get_argument(para, "-ad", ",")[0];
    imp.warning = strcmp(get_argument(para, "-w", "true").c_str(), "true") == 0;
    imp.error = strcmp(get_argument(para, "-err", "true").c_str(), "true") == 0;
    bool info =
        strcmp(get_argument(para, "-info", "true").c_str(), "true") == 0;

    // IMPORT VERTICES
    size_t l_v = 0;
    auto o = take_argument(para, "-v");
    while (o.is_present()) {
        std::fstream file(o.get());

        if (info)
            std::cout << "Importing vertices from file: " << o.get()
                      << std::endl;

        auto n = imp.import_vertices(file);
        l_v = +n;

        if (info)
            std::cout << "Loaded " << n << " vertices from " << o.get()
                      << std::endl;

        o = take_argument(para, "-v");
    }

    // IMPORT EDGES
    size_t l_e = 0;
    o = take_argument(para, "-e");
    while (o.is_present()) {
        std::fstream file(o.get());

        if (info)
            std::cout << "Importing edges from file: " << o.get() << std::endl;

        auto n = imp.import_edges(file);
        l_e = +n;

        if (info)
            std::cout << "Loaded " << n << " edges from " << o.get()
                      << std::endl;

        o = take_argument(para, "-e");
    }

    t.commit();

    return std::make_pair(l_v, l_e);
}
