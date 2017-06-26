#ifndef PLANNING_CPP_PLAN_H
#define PLANNING_CPP_PLAN_H


#include "trajectory.h"
#include "visibility.h"

struct Plan;
typedef shared_ptr<Plan> PPlan;

struct Plan {
    const TimeWindow time_window;
    vector<Trajectory> trajectories;
    shared_ptr<FireData> fire;
    vector<PointTimeWindow> possible_observations;

    Plan(const Plan& plan) = default;

    Plan(vector<TrajectoryConfig> traj_confs, shared_ptr<FireData> fire_data, TimeWindow tw)
            : time_window(tw), fire(fire_data)
    {
        for(auto conf : traj_confs) {
            ASSERT(conf.start_time >= time_window.start && conf.start_time <= time_window.end);
            auto traj = Trajectory(conf);
            trajectories.push_back(traj);
        }

        for(size_t x=0; x<fire->ignitions.x_width; x++) {
            for (size_t y = 0; y < fire->ignitions.y_height; y++) {
                const double t = fire->ignitions(x, y);
                if (time_window.start <= t && t <= time_window.end) {
                    Cell c{x, y};
                    possible_observations.push_back(
                            PointTimeWindow{fire->ignitions.as_point(c), {fire->ignitions(c), fire->traversal_end(c)}});
                }
            }
        }
    }

    /** A plan is valid iff all trajectories are valid (match their configuration. */
    bool is_valid() const {
        for(auto traj : trajectories)
            if(!traj.is_valid())
                return false;
        return true;
    }

    /** SUm of all trajectory durations. */
    double duration() const {
        double duration = 0;
        for(auto it=trajectories.begin(); it!=trajectories.end(); it++) {
            duration += it->duration();
        }
        return duration;
    }

    /** Cost of the plan.
     * The key idea is to sum the distance of all ignited points in the time window to their closest observation.
     **/
    double cost() const {
        vector<PointTime> done_obs = observations();
        double global_cost = 0;
        for(PointTimeWindow possible_obs : possible_observations) {
            double min_dist = MAX_INFORMATIVE_DISTANCE;
            // find the closest observation.
            for(PointTime obs : done_obs) {
                min_dist = min(min_dist, possible_obs.pt.dist(obs.pt));
            }
            // cost is based on the minimal distance to the observation and normalized such that
            // cost = 0 if min_dist <= REDUNDANT_OBS_DIT
            // cost = 1 if min_dist = (MAX_INFORMATIVE_DISTANCE - REDUNDANT_OBS_DIST
            // evolves linearly in between.
            const double self_cost = (max(min_dist, REDUNDANT_OBS_DIST)-REDUNDANT_OBS_DIST) / (MAX_INFORMATIVE_DISTANCE - REDUNDANT_OBS_DIST);
            global_cost += self_cost;
        }
        return global_cost;
    }

    size_t num_segments() const {
        size_t total = 0;
        for(auto traj : trajectories)
            total += traj.traj.size();
        return total;
    }

    /** Returns the UAV performing the given trajectory */
    UAV uav(size_t traj_id) const {
        ASSERT(traj_id < trajectories.size())
        return trajectories[traj_id].conf.uav;
    }

    /** All observations in the plan. Computed by taking the visibility center of all segments.
     * Each observation is tagged with a time, corresponding to the start time of the segment.*/
    vector<PointTime> observations() const {
        vector<PointTime> obs;
        for(auto traj : trajectories) {
            for(size_t seg_id=0; seg_id<traj.size(); seg_id++) {
                auto seg = traj[seg_id];
                auto wp = traj.conf.uav.visibilty_center(seg);
                double obs_time = traj.start_time(seg_id);
                Cell c = fire->ignitions.as_cell(wp);
                if(fire->ignitions(c) <= obs_time && obs_time <= fire->traversal_end(c)) {
                    // this observation overlaps the firefront, add it to the valid observations.
                    PointTime ptt{Point{wp.x, wp.y}, traj.start_time(seg_id)};
                    obs.push_back(ptt);
                }
            }
        }
        return obs;
    }


    void insert_segment(size_t traj_id, const Segment& seg, size_t insert_loc) {
        ASSERT(traj_id < trajectories.size());
        ASSERT(insert_loc <= trajectories[traj_id].traj.size());
        trajectories[traj_id].insert_segment(seg, insert_loc);
        project_on_firefront();
    }

    void erase_segment(size_t traj_id, size_t at_index) {
        ASSERT(traj_id < trajectories.size());
        ASSERT(at_index < trajectories[traj_id].traj.size());
        Segment deleted = trajectories[traj_id][at_index];
        trajectories[traj_id].erase_segment(at_index);
        project_on_firefront();
    }

    void replace_segment(size_t traj_id, size_t at_index, const Segment& by_segment) {
        ASSERT(traj_id < trajectories.size());
        ASSERT(at_index <= trajectories[traj_id].traj.size());
        erase_segment(traj_id, at_index);
        insert_segment(traj_id, by_segment, at_index);
        project_on_firefront();
    }

    void project_on_firefront() {
        for(auto &traj : trajectories) {
            size_t seg_id = 0;
            while(seg_id < traj.size()) {
                const Segment& seg = traj[seg_id];
                const double t = traj.start_time(seg_id);
                opt<Segment> projected = fire->project_on_firefront(seg, traj.conf.uav, t);
                if(projected) {
                    if(*projected != seg) {
                        // projection is different than original, replace it if the projection is not to close to the previous/next segment
                        auto curr_pt =  traj.conf.uav.visibilty_center(*projected).as_point();
                        // distance to previous/next observations (large number if there are no previous/next)
                        const double prev_point_dist = seg_id == 0 ? 999999 :
                                                       curr_pt.dist(traj.conf.uav.visibilty_center(traj[seg_id-1]).as_point());
                        const double next_point_dist = seg_id >= traj.size()-1 ? 999999
                                                                               : curr_pt.dist(traj.conf.uav.visibilty_center(traj[seg_id+1]).as_point());
                        traj.erase_segment(seg_id);
                        // only reinsert, if it is not to close to the next/previous
                        if(prev_point_dist > 49. && next_point_dist > 49.) {
                            traj.insert_segment(*projected, seg_id);
                            seg_id++;
                        }
                    } else {
                        // nothing change for this segment, go to next
                        seg_id++;
                    }
                } else {
                    // segment has no projection, remove it
                    traj.erase_segment(seg_id);
                }
            }
        }
    }

private:
    /** Constants used for computed the cost associated to a pair of points.
     * The cost is MAX_INDIVIDUAL_COST if the distance between two points
     * is >= MAX_INFORMATIVE_DISTANCE. It is be 0 if the distance is 0 and scales linearly between the two. */
    const double MAX_INFORMATIVE_DISTANCE = 500.;

    /** If a point is less than REDUNDANT_OBS_DIST aways from another observation, it useless to observe it.
     * This is defined such that those point are in the visible area when pictured. */
    const double REDUNDANT_OBS_DIST = 0.;
};



#endif //PLANNING_CPP_PLAN_H
