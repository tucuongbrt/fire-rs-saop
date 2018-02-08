/* Copyright (c) 2017, CNRS-LAAS
All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions are met:

 * Redistributions of source code must retain the above copyright notice, this
   list of conditions and the following disclaimer.

 * Redistributions in binary form must reproduce the above copyright notice,
   this list of conditions and the following disclaimer in the documentation
   and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE. */

#ifndef PLANNING_CPP_PYTHON_VNS_H
#define PLANNING_CPP_PYTHON_VNS_H

#include <pybind11/pybind11.h>

#include <pybind11/stl.h> // for conversions between c++ and python collections
#include <pybind11/numpy.h> // support for numpy arrays
#include "core/trajectory.hpp"
#include "core/raster.hpp"
#include "vns/factory.hpp"
#include "firemapping/ghostmapper.hpp"

#include <cstdlib>
#include <cstring>
#include <iostream>

namespace py = pybind11;

/** Converts a numpy array to a vector */
template<class T>
std::vector<T> as_vector(py::array_t<T, py::array::c_style | py::array::forcecast> array) {
    std::vector<T> data(array.size());
    for (ssize_t x = 0; x < array.shape(0); x++) {
        for (ssize_t y = 0; y < array.shape(1); y++) {
            data[x + y * array.shape(0)] = *(array.data(x, y));
        }
    }
    return data;
}

/** Converts a vector to a 2D numpy array. */
template<class T>
py::array_t<T> as_nparray(std::vector<T> vec, size_t x_width, size_t y_height) {
    ASSERT(vec.size() == x_width * y_height)
    py::array_t<T, py::array::c_style | py::array::forcecast> array(std::vector<size_t> { x_width, y_height });
    auto s_x_width = static_cast<ssize_t>(x_width);
    auto s_y_height = static_cast<ssize_t>(y_height);
    for (ssize_t x = 0; x < s_x_width; x++) {
        for (ssize_t y = 0; y < s_y_height; y++) {
            *(array.mutable_data(x, y)) = vec[x + y * x_width];
        }
    }
    return array;
}

namespace SAOP {
    SearchResult
    plan_vns(vector<TrajectoryConfig> configs, DRaster ignitions, DRaster elevation, const std::string& json_conf) {
        auto time = []() {
            struct timeval tp;
            gettimeofday(&tp, NULL);
            return (double) tp.tv_sec + ((double) (tp.tv_usec / 1000) / 1000.);
        };
        json conf = json::parse(json_conf);
        SAOP::check_field_is_present(conf, "min_time");
        const double min_time = conf["min_time"];
        SAOP::check_field_is_present(conf, "max_time");
        const double max_time = conf["max_time"];
        SAOP::check_field_is_present(conf, "save_every");
        const size_t save_every = conf["save_every"];
        SAOP::check_field_is_present(conf, "save_improvements");
        const bool save_improvements = conf["save_improvements"];
        SAOP::check_field_is_present(conf, "vns");
        SAOP::check_field_is_present(conf["vns"], "max_time");
        const size_t max_planning_time = conf["vns"]["max_time"];

        std::cout << "Processing firedata data" << std::endl;
        double preprocessing_start = time();
        shared_ptr<FireData> fire_data = make_shared<FireData>(ignitions, elevation);
        double preprocessing_end = time();

        std::cout << "Building initial plan" << std::endl;
        Plan p(configs, fire_data, TimeWindow{min_time, max_time});

        std::cout << "Planning" << std::endl;
        auto vns = build_from_config(conf["vns"].dump());
        const double planning_start = time();
        auto res = vns->search(p, max_planning_time, save_every, save_improvements);
        const double planning_end = time();

        std::cout << "Plan found in " << planning_end - planning_start << " seconds" << std::endl;
        std::cout << "Best plan: utility: " << res.final_plan->utility()
                  << " -- duration:" << res.final_plan->duration() << std::endl;
        res.metadata["planning_time"] = planning_end - planning_start;
        res.metadata["preprocessing_time"] = preprocessing_end - preprocessing_start;
        res.metadata["configuration"] = conf;
        return res;
    }

    SearchResult
    replan_vns(SearchResult last_search, double after_time, DRaster ignitions, DRaster elevation,
               const std::string& json_conf) {
        auto time = []() {
            struct timeval tp;
            gettimeofday(&tp, NULL);
            return (double) tp.tv_sec + ((double) (tp.tv_usec / 1000) / 1000.);
        };
        json conf = json::parse(json_conf);
        SAOP::check_field_is_present(conf, "min_time");
        const double min_time = conf["min_time"];
        SAOP::check_field_is_present(conf, "max_time");
        const double max_time = conf["max_time"];
        SAOP::check_field_is_present(conf, "save_every");
        const size_t save_every = conf["save_every"];
        SAOP::check_field_is_present(conf, "save_improvements");
        const bool save_improvements = conf["save_improvements"];
        SAOP::check_field_is_present(conf, "vns");
        SAOP::check_field_is_present(conf["vns"], "max_time");
        const size_t max_planning_time = conf["vns"]["max_time"];

        std::cout << "Processing updated fire data" << std::endl;
        double preprocessing_start = time();
        shared_ptr<FireData> fire_data = make_shared<FireData>(ignitions, elevation);
        double preprocessing_end = time();

        std::cout << "Building initial plan from last final plan" << std::endl;
        Plan p = last_search.final();
        p.firedata = fire_data;
        p.trajectories.freeze_before(after_time);
        p.project_on_fire_front();

        std::cout << "Planning" << std::endl;
        auto vns = build_from_config(conf["vns"].dump());
        const double planning_start = time();
        auto res = vns->search(p, max_planning_time, save_every, save_improvements);
        const double planning_end = time();

        res.metadata["planning_time"] = planning_end - planning_start;
        res.metadata["preprocessing_time"] = preprocessing_end - preprocessing_start;
        res.metadata["configuration"] = conf;

        std::cout << "Plan found in " << planning_end - planning_start << " seconds" << std::endl;
        std::cout << "Best plan: utility: " << res.final_plan->utility()
                  << " -- duration:" << res.final_plan->duration() << std::endl;

        return res;
    }
}


PYBIND11_DECLARE_HOLDER_TYPE(T, std::shared_ptr<T>);

PYBIND11_MODULE(uav_planning, m) {
    m.doc() = "Python module for UAV trajectory planning";

    srand(0);
#ifdef DEBUG
    std::cerr << "Warning: Planning module compiled in debug mode. Expect slowness ;)\n";
#endif

    py::class_<DRaster>(m, "DRaster")
            .def(py::init([](py::array_t<double, py::array::c_style | py::array::forcecast> arr,
                             double x_offset, double y_offset, double cell_width) {
                return new DRaster(as_vector<double>(arr), arr.shape(0), arr.shape(1), x_offset, y_offset, cell_width);
            }))
            .def("as_numpy", [](DRaster& self) {
                return as_nparray<double>(self.data, self.x_width, self.y_height);
            })
            .def_readonly("x_offset", &DRaster::x_offset)
            .def_readonly("y_offset", &DRaster::y_offset)
            .def_readonly("cell_width", &DRaster::cell_width);

    py::class_<LRaster>(m, "LRaster")
            .def(py::init([](py::array_t<long, py::array::c_style | py::array::forcecast> arr,
                             double x_offset, double y_offset, double cell_width) {
                return new LRaster(as_vector<long>(arr), arr.shape(0), arr.shape(1), x_offset, y_offset, cell_width);
            }))
            .def("as_numpy", [](LRaster& self) {
                return as_nparray<long>(self.data, self.x_width, self.y_height);
            })
            .def_readonly("x_offset", &LRaster::x_offset)
            .def_readonly("y_offset", &LRaster::y_offset)
            .def_readonly("cell_width", &LRaster::cell_width);

    py::class_<TimeWindow>(m, "TimeWindow")
            .def(py::init<const double, const double>(),
                 py::arg("start"), py::arg("end"))
            .def_readonly("start", &TimeWindow::start)
            .def_readonly("end", &TimeWindow::end)
            .def("contains", (bool (TimeWindow::*)(double time) const) &TimeWindow::contains,
                 py::arg("time"))
            .def("contains", (bool (TimeWindow::*)(const TimeWindow&) const) &TimeWindow::contains,
                 py::arg("time_window"))
            .def("__repr__",
                 [](const TimeWindow& tw) {
                     std::stringstream repr;
                     repr << "TimeWindow(" << tw.start << ", " << tw.end << ")";
                     return repr.str();
                 }
            )
            .def("as_tuple", [](TimeWindow& self) {
                return py::make_tuple(self.start, self.end);
            });

    py::class_<Cell>(m, "Cell")
            .def(py::init<const size_t, const size_t>(),
                 py::arg("x"), py::arg("y"))
            .def_readonly("x", &Cell::x)
            .def_readonly("y", &Cell::y)
            .def("__repr__",
                 [](const Cell& c) {
                     std::stringstream repr;
                     repr << "Cell(" << c.x << ", " << c.y << ")";
                     return repr.str();
                 }
            )
            .def("as_tuple", [](Cell& self) {
                return py::make_tuple(self.x, self.y);
            });

    py::class_<Position>(m, "Position2d")
            .def(py::init<double, double>(),
                 py::arg("x"), py::arg("y"))
            .def_readonly("x", &Position::x)
            .def_readonly("y", &Position::y)
            .def("__repr__",
                 [](const Position& p) {
                     std::stringstream repr;
                     repr << "Position2d(" << p.x << ", " << p.y << ")";
                     return repr.str();
                 }
            )
            .def("as_tuple", [](Position& self) {
                return py::make_tuple(self.x, self.y);
            });

    py::class_<Position3d>(m, "Position")
            .def(py::init<double, double, double>(),
                 py::arg("x"), py::arg("y"), py::arg("z"))
            .def_readonly("x", &Position3d::x)
            .def_readonly("y", &Position3d::y)
            .def_readonly("z", &Position3d::z)
            .def("__repr__",
                 [](const Position3d& p) {
                     std::stringstream repr;
                     repr << "Position(" << p.x << ", " << p.y << ", " << p.z << ")";
                     return repr.str();
                 }
            )
            .def("as_tuple", [](Position3d& self) {
                return py::make_tuple(self.x, self.y, self.z);
            });

    py::class_<PositionTime>(m, "Position2dTime")
            .def(py::init<Position, double>(),
                 py::arg("point"), py::arg("time"))
            .def_readonly("pt", &PositionTime::pt)
            .def_readonly("time", &PositionTime::time)
            .def("__repr__",
                 [](const PositionTime& p) {
                     std::stringstream repr;
                     repr << "Position2dTime(" << p.pt.x << ", " << p.pt.y << ", " << p.time << ")";
                     return repr.str();
                 }
            )
            .def("as_tuple", [](PositionTime& self) {
                return py::make_tuple(py::make_tuple(self.pt.x, self.pt.y), self.time);
            });

    py::class_<Position3dTime>(m, "PositionTime")
            .def(py::init<Position3d, double>(),
                 py::arg("point"), py::arg("time"))
            .def_readonly("pt", &Position3dTime::pt)
            .def_readonly("time", &Position3dTime::time)
            .def("__repr__",
                 [](const Position3dTime& p) {
                     std::stringstream repr;
                     repr << "PositionTime(" << p.pt.x << ", " << p.pt.y << ", " << p.pt.z << ", " << p.time << ")";
                     return repr.str();
                 }
            )
            .def("as_tuple", [](Position3dTime& self) {
                return py::make_tuple(py::make_tuple(self.pt.x, self.pt.y, self.pt.z), self.time);
            });

    py::class_<FireData, std::shared_ptr<FireData>>(m, "FireData")
            .def(py::init<const DRaster&, const DRaster&>(), py::arg("ignitions"), py::arg("elevation"))
            .def_readonly("ignitions", &FireData::ignitions)
            .def_readonly("traversal_end", &FireData::traversal_end)
            .def_readonly("propagation_directions", &FireData::propagation_directions)
            .def_readonly("elevation", &FireData::elevation);

    py::class_<Waypoint3d>(m, "Waypoint")
            .def(py::init<const double, const double, const double, const double>(),
                 py::arg("x"), py::arg("y"), py::arg("z"), py::arg("direction"))
            .def_readonly("x", &Waypoint3d::x)
            .def_readonly("y", &Waypoint3d::y)
            .def_readonly("z", &Waypoint3d::z)
            .def_readonly("dir", &Waypoint3d::dir)
            .def("__repr__", &Waypoint3d::to_string);

    py::class_<Segment3d>(m, "Segment")
            .def(py::init<const Waypoint3d, const double>())
            .def(py::init<const Waypoint3d, const Waypoint3d>())
            .def_readonly("start", &Segment3d::start)
            .def_readonly("end", &Segment3d::end)
            .def_readonly("length", &Segment3d::length)
            .def("__repr__", &Segment3d::to_string);

    py::class_<UAV>(m, "UAV")
            .def(py::init<const double, const double, const double>())
            .def_property_readonly("min_turn_radius", &UAV::min_turn_radius)
            .def_property_readonly("max_air_speed", &UAV::max_air_speed)
            .def_property_readonly("max_pitch_angle", &UAV::max_pitch_angle)
            .def("travel_distance", (double (UAV::*)(const Waypoint3d&, const Waypoint3d&) const)
                    &UAV::travel_distance, py::arg("origin"), py::arg("destination"))
            .def("travel_distance", (double (UAV::*)(const Waypoint&, const Waypoint&) const)
                    &UAV::travel_distance, py::arg("origin"), py::arg("destination"))
            .def("travel_time", (double (UAV::*)(const Waypoint3d&, const Waypoint3d&) const)
                    &UAV::travel_time, py::arg("origin"), py::arg("destination"))
            .def("travel_time", (double (UAV::*)(const Waypoint&, const Waypoint&) const)
                    &UAV::travel_time, py::arg("origin"), py::arg("destination"))
            .def("path_sampling",
                 (std::vector<Waypoint3d> (UAV::*)(const Waypoint3d&, const Waypoint3d&, const double) const)
                         &UAV::path_sampling, py::arg("origin"), py::arg("destination"), py::arg("step_size"));

    py::class_<Trajectory>(m, "Trajectory")
            .def(py::init<const TrajectoryConfig&>())
            .def_property_readonly("conf", &Trajectory::conf)
            .def("start_time", (double (Trajectory::*)() const) &Trajectory::start_time)
            .def("start_time", (double (Trajectory::*)(size_t) const) &Trajectory::start_time, py::arg("segment_index"))
            .def("end_time", (double (Trajectory::*)() const) &Trajectory::end_time)
            .def("end_time", (double (Trajectory::*)(size_t) const) &Trajectory::start_time, py::arg("segment_index"))
            .def_property_readonly("segments", &Trajectory::maneuvers)
            .def("segment", &Trajectory::maneuver, py::arg("index"))
            .def_property_readonly("start_times", &Trajectory::start_times)
            .def_property_readonly("modifiable", &Trajectory::modifiable)
            .def("can_modify", &Trajectory::can_modify, py::arg("maneuver_index"))
            .def("first_modifiable_id", &Trajectory::first_modifiable_maneuver)
            .def("slice", (Trajectory (Trajectory::*)(TimeWindow) const) &Trajectory::slice, py::arg("time_window"))
            .def("slice", [](Trajectory& self, py::tuple range) -> Trajectory {
                auto tw = TimeWindow(range[0].cast<double>(), range[1].cast<double>());
                return self.slice(tw);
            }, py::arg("time_window"))
            .def("slice", &Trajectory::slice, py::arg("time_window"))
            .def("length", &Trajectory::length)
            .def("__len__", &Trajectory::size)
            .def("duration", &Trajectory::duration)
            .def("as_waypoints", &Trajectory::as_waypoints)
            .def("sampled", &Trajectory::sampled, py::arg("step_size") = 1)
            .def("sampled_with_time", (std::pair<std::vector<Waypoint3d>, std::vector<double>> (Trajectory::*)(
                    double) const) &Trajectory::sampled_with_time, py::arg("step_size") = 1)
            .def("sampled_with_time", (std::pair<std::vector<Waypoint3d>, std::vector<double>> (Trajectory::*)
                         (TimeWindow, double) const) &Trajectory::sampled_with_time,
                 py::arg("time_range"), py::arg("step_size") = 1)
            .def("sampled_with_time", [](Trajectory& self, py::tuple time_range,
                                         double step) -> std::pair<std::vector<Waypoint3d>, std::vector<double>> {
                     return self.sampled_with_time(TimeWindow(
                             time_range[0].cast<double>(), time_range[1].cast<double>()), step);
                 },
                 py::arg("time_range"), py::arg("step_size") = 1)
            .def("with_waypoint_at_end", &Trajectory::with_waypoint_at_end)
            .def("__repr__", &Trajectory::to_string)
            .def("trace", [](Trajectory& self, const DRaster& r) {
                vector<PositionTime> trace = vector<PositionTime>{};
                for (auto i = 0ul; i <= self.size(); ++i) {
                    Plan::segment_trace(self[i].maneuver, self.conf().uav.view_width(), self.conf().uav.view_depth(), r);
                }
                return trace;
            }, py::arg("raster"));

    py::class_<TrajectoryConfig>(m, "TrajectoryConfig")
            .def(py::init<UAV, Waypoint3d, Waypoint3d, double, double>())
            .def_readonly("uav", &TrajectoryConfig::uav)
            .def_readonly("max_flight_time", &TrajectoryConfig::max_flight_time)
            .def_static("build", [](UAV uav, double start_time, double max_flight_time) -> TrajectoryConfig {
                            return TrajectoryConfig(uav, start_time, max_flight_time);
                        }, "Constructor", py::arg("uav"), py::arg("start_time") = 0,
                        py::arg("max_flight_time") = std::numeric_limits<double>::max());

    py::class_<Plan>(m, "Plan")
            .def("trajectories", [](Plan& self) { return self.trajectories.trajectories; })
            .def("utility", &Plan::utility)
            .def("duration", &Plan::duration)
            .def_readonly("firedata", &Plan::firedata)
            .def_readonly("time_window", &Plan::time_window)
            .def("observations", (vector<PositionTime> (Plan::*)() const) &Plan::observations)
            .def("observations", (vector<PositionTime> (Plan::*)(const TimeWindow&) const) &Plan::observations,
                 py::arg("tw"))
            .def("view_trace", (vector<PositionTime> (Plan::*)() const) &Plan::view_trace)
            .def("view_trace", (vector<PositionTime> (Plan::*)(const TimeWindow&) const) &Plan::view_trace,
                 py::arg("tw"));

    py::class_<SearchResult>(m, "SearchResult")
            .def("initial_plan", &SearchResult::initial)
            .def("final_plan", &SearchResult::final)
            .def_readonly("intermediate_plans", &SearchResult::intermediate_plans)
            .def("metadata", [](SearchResult& self) { return self.metadata.dump(); });


    m.def("replan_vns", SAOP::replan_vns, py::arg("last_search"), py::arg("after_time"), py::arg("ignitions_update"),
          py::arg("elevation_update"), py::arg("json_conf"), py::call_guard<py::gil_scoped_release>());


    m.def("plan_vns", SAOP::plan_vns, py::arg("trajectory_configs"), py::arg("ignitions"), py::arg("elevation"),
          py::arg("json_conf"), py::call_guard<py::gil_scoped_release>());
}

#endif //PLANNING_CPP_PYTHON_VNS_H