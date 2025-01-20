#include "railx_2d_twisted_torus.h"


/**
 * Constructor of RailX2DTwistedTorus
 */
RailX2DTwistedTorus::RailX2DTwistedTorus() : num_mesh_(num_groups_), meshes_(groups_) {
  read_config();
  num_mesh_ = x_scale_ * y_scale_;
  num_nodes_ = num_mesh_ * (m_scale_ * m_scale_);
  num_cores_ = num_nodes_;
  meshes_.reserve(num_mesh_);

  for (int mesh_id = 0; mesh_id < num_mesh_; mesh_id++) {
    meshes_.push_back(new HBMesh(m_scale_, n_port_, param->vc_number, param->buffer_size,
                                 internal_HB_link, external_link));
    meshes_[mesh_id]->set_group(this, mesh_id);
    get_mesh(mesh_id)->coordinate_ = {mesh_id % x_scale_, mesh_id / x_scale_};
  }
  connect();
}

/**
 * Destructor of RailX2DTwistedTorus
 */
RailX2DTwistedTorus::~RailX2DTwistedTorus() {
  for (auto mesh : meshes_) delete mesh;
  meshes_.clear();
}


/**
 * Read the configuration of RailX2DTwistedTorus
 */
void RailX2DTwistedTorus::read_config() {
  m_scale_ = param->params_ptree.get<int>("Network.m_scale", 4);
  n_port_ = param->params_ptree.get<int>("Network.n_port", 1);
  x_scale_ = param->params_ptree.get<int>("Network.x_scale", 1);
  y_scale_ = param->params_ptree.get<int>("Network.y_scale", 2);
  assert(x_scale_ == y_scale_ * 2);
  algorithm_ = param->params_ptree.get<std::string>("Network.routing_algorithm", "MIN");
  int internal_bandiwdth = param->params_ptree.get<int>("Network.internal_bandwidth", 1);
  int external_latency = param->params_ptree.get<int>("Network.external_latency", 1);
  internal_HB_link = Channel(internal_bandiwdth, 1);
  external_link = Channel(1, external_latency);
}


/**
 * Connect the twisted torus
 * Reference:
 * https://ieeexplore.ieee.org/stamp/stamp.jsp?arnumber=5406510
 */
void RailX2DTwistedTorus::connect()
{
    for (int mesh_id = 0; mesh_id < num_mesh_; ++mesh_id)
    {
        HBMesh* mesh = get_mesh(mesh_id);
        int mesh_x = mesh->coordinate_[0];
        int mesh_y = mesh->coordinate_[1];
        ChipletInMesh* chiplet;

        int link_chiplet_id, link_mesh_id;

        // ===========================
        // Connects the x direction neighbors
        // ===========================
        for (int chiplet_y = 0; chiplet_y < m_scale_; ++chiplet_y)
        {   
            // **************************
            // The x-neg link connects to the x-pos link of left neighbor
            // **************************

            // chiplet_y * m_scale_ is the index of the left most chiplet in the row
            chiplet = mesh->get_chiplet(chiplet_y * m_scale_);     // x-neg
            // link_chiplet_id is the index of the right most chiplet in the row
            link_chiplet_id = chiplet_y * m_scale_ + m_scale_ - 1; // x_chiplet = m_scale_ -1
            // link_mesh_id is the index of the mesh to the left of the current mesh
            link_mesh_id = mesh_y * x_scale_ + (mesh_id - 1 + x_scale_) % x_scale_;
            // Connects the x-neg link to the x-pos link of the chiplet at the other end of the row
            for (int i = 0; i < n_port_; ++i)
            {
                chiplet->xneg_link_nodes_[i].get() = NodeID(link_chiplet_id, link_mesh_id);
                chiplet->xneg_link_buffers_[i] = get_chiplet(chiplet->xneg_link_nodes_[i])->xpos_in_buffers_[i];
            }

            // **************************
            // The x-pos link connects to the x-neg link of right neighbor
            // **************************
            chiplet = mesh->get_chiplet(chiplet_y * m_scale_ + m_scale_ - 1);  // x-pos
            link_chiplet_id = chiplet_y * m_scale_;                            // x_chiplet = 0
            link_mesh_id = mesh_y * x_scale_ + (mesh_id + 1) % x_scale_;
            for (int i = 0; i < n_port_; ++i)
            {
                chiplet->xpos_link_nodes_[i].get() = NodeID(link_chiplet_id, link_mesh_id);
                chiplet->xpos_link_buffers_[i] = get_chiplet(chiplet->xpos_link_nodes_[i])->xneg_in_buffers_[i];
            }
        }

        // ===========================
        // Connects the y direction neighbors
        // ===========================

        for (int chiplet_x = 0; chiplet_x < m_scale_; ++chiplet_x)
        {   
            // FIXME: Can probably be simpler, written this way for clarity
            // **************************
            // Connects the y-neg link
            // **************************
            chiplet = mesh->get_chiplet(chiplet_x);                   // y-neg
            link_chiplet_id = chiplet_x + (m_scale_ - 1) * m_scale_;  // y_chiplet = m_scale_ -1
            if (mesh_y > 0)
            {
                // if the current mesh is not the bottom most mesh (mesh_y > 0)
                // the y-neg link connects to the y-pos link of the mesh below
                link_mesh_id = mesh_id - x_scale_;
                assert(link_mesh_id >= 0);
            }
            else
            {
                // if the current mesh is the bottom most mesh (mesh_y == 0)
                // i.e., the wrap around case
                if (mesh_x >= 0 && mesh_x < y_scale_)
                {   
                    // if the current mesh is on the left side of the torus (0 <= mesh_x < y_scale_)
                    // the y-neg link connects to the y-pos link of (mesh_x + y_scale_, y_scale_ - 1)
                    link_mesh_id = (y_scale_ - 1) * x_scale_ + mesh_x + y_scale_;
                }
                else
                {
                    // if the current mesh is on the right side of the torus (y_scale_ <= mesh_x < x_scale_)
                    // the y-neg link connects to the y-pos link of (mesh_x - y_scale_, y_scale_ - 1)
                    link_mesh_id = (y_scale_ - 1) * x_scale_ + mesh_x - y_scale_;
                }
            }
            for (int i = 0; i < n_port_; ++i)
            {
                chiplet->yneg_link_nodes_[i].get() = NodeID(link_chiplet_id, link_mesh_id);
                chiplet->yneg_link_buffers_[i] = get_chiplet(chiplet->yneg_link_nodes_[i])->ypos_in_buffers_[i];
            }
            
            // **************************
            // Connects the y-pos link
            // **************************
            chiplet = mesh->get_chiplet(chiplet_x + (m_scale_ - 1) * m_scale_);  // y-pos
            link_chiplet_id = chiplet_x;                                         // y_chiplet = 0
            if (mesh_y < y_scale_ - 1)
            {
                // if the current mesh is not the top most mesh (mesh_y < y_scale_ - 1)
                // the y-pos link connects to the y-neg link of the mesh above
                link_mesh_id = mesh_id + x_scale_;
                assert(link_mesh_id < num_mesh_);
            }
            else
            {
                // if the current mesh is the top most mesh (mesh_y == y_scale_ - 1)
                // i.e., the wrap around case
                if (mesh_x >= 0 && mesh_x < y_scale_)
                {   
                    // if the current mesh is on the left side of the torus (0 <= mesh_x < y_scale_)
                    // the y-pos link connects to the y-neg link of (mesh_x + y_scale_, 0)
                    link_mesh_id = mesh_x + y_scale_;
                }
                else
                {
                    // if the current mesh is on the right side of the torus (y_scale_ <= mesh_x < x_scale_)
                    // the y-pos link connects to the y-neg link of (mesh_x - y_scale_, 0)
                    link_mesh_id = mesh_x - y_scale_;
                }
            }
            for (int i = 0; i < n_port_; ++i)
            {
                chiplet->ypos_link_nodes_[i].get() = NodeID(link_chiplet_id, link_mesh_id);
                chiplet->ypos_link_buffers_[i] = get_chiplet(chiplet->ypos_link_nodes_[i])->yneg_in_buffers_[i];
            }
        }
    }
}

/**
 * Routing algorithm for RailX2DTwistedTorus
 * TODO Implement more routing algorithms
 */
void RailX2DTwistedTorus::routing_algorithm(Packet& s) const
{
    if (algorithm_ == "XY")
        XY_routing(s);
    else
        std::cerr << "Unknown routing algorithm: " << algorithm_ << std::endl;
}


/**
 * Implements the XY routing algorithm for RailX2DTwistedTorus
 * Algorithm is implemented based on Algorithm 1 
 * described in the twisted torus paper:
 * https://ieeexplore.ieee.org/document/5406510
 */
void RailX2DTwistedTorus::XY_routing(Packet& s) const
{
    ChipletInMesh* current_chiplet = get_chiplet(s.head_trace().id);
    ChipletInMesh* destination_chiplet = get_chiplet(s.destination_);
    HBMesh* current_mesh = get_mesh(s.head_trace().id.group_id);
    HBMesh* dest_mesh = get_mesh(s.destination_.group_id);

    int cur_x = current_mesh->coordinate_[0] * m_scale_ + current_chiplet->x_;
    int cur_y = current_mesh->coordinate_[1] * m_scale_ + current_chiplet->y_;

    int dest_x = dest_mesh->coordinate_[0] * m_scale_ + destination_chiplet->x_;
    int dest_y = dest_mesh->coordinate_[1] * m_scale_ + destination_chiplet->y_;

    int dis_x = dest_x - cur_x;  // x offset
    int dis_y = dest_y - cur_y;  // y offset

    if (dis_x == 0 && dis_y == 0)
    {
        std::cerr << "The source and destination are the same!" << std::endl;
        return;
    }
    assert(x_scale_ / 2 == y_scale_);
    unsigned int a = x_scale_ * m_scale_ / 2;

    // Computes all possible coordinate differences (dx, dy) that the
    // twisted torus wraparounds allow
    // In a 2a x a twisted torus, x can wrap by +/- a or +/- 2a, while
    // y can wrap by +/- a
    const int NUM_CANDIDATES = 7;
    // FIXME: Should probably not hardcode the order of the pairs
    int dx_candidates[NUM_CANDIDATES] = {
        dest_x - cur_x,
        dest_x - cur_x - a,
        dest_x - cur_x + a,
        dest_x - cur_x + 2 * a,
        dest_x - cur_x - 2 * a,
        dest_x - cur_x + a,
        dest_x - cur_x - a
    };

    int dy_candidates[NUM_CANDIDATES] = {
        dest_y - cur_y,
        dest_y - cur_y - a,
        dest_y - cur_y - a,
        dest_y - cur_y,
        dest_y - cur_y,
        dest_y - cur_y + a,
        dest_y - cur_y + a
    };

    // Finds which dx, dy pair has the smallest Manhattan distance
    int min_dx = 0;
    int min_dy = 0;
    // x_wrap and y_wrap are set to true to represent
    // the case where the destination will be reached by using the
    // wrap around links, and false otherwise
    bool x_wrap = true;
    bool y_wrap = true;

    int best_dist = std::numeric_limits<int>::max();
    for (int i = 0; i < NUM_CANDIDATES; i++)
    {
        int dist = abs(dx_candidates[i]) + abs(dy_candidates[i]);
        if (dist < best_dist)
        {
            best_dist = dist;
            min_dx = dx_candidates[i];
            min_dy = dy_candidates[i];

            if (i == 1)
            {
                x_wrap = false;
                y_wrap = false;
            }
            else if (i == 2 || i == 3)
            {
                y_wrap = false;
            } 
        }
    }

    // Pushes the candidate channels based on the best dx, dy pair
    // FIXME: If wrap around links are used, the VC is set to 1,
    // otherwise it is set to 0. Not sure if this is correct
    int vcb = x_wrap ? 1 : 0;
    if (min_dx < 0)
    {
        for (int i = 0; i < n_port_; i++)
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xneg_link_buffers_[i], vcb));
    }
    else if (min_dx > 0)
    {
        for (int i = 0; i < n_port_; i++)
            s.candidate_channels_.push_back(VCInfo(current_chiplet->xpos_link_buffers_[i], vcb));
    }

    vcb = y_wrap ? 1 : 0;
    if (min_dy < 0)
    {
        for (int i = 0; i < n_port_; i++)
            s.candidate_channels_.push_back(VCInfo(current_chiplet->yneg_link_buffers_[i], vcb));
    }
    else if (min_dy > 0)
    {
        for (int i = 0; i < n_port_; i++)
            s.candidate_channels_.push_back(VCInfo(current_chiplet->ypos_link_buffers_[i], vcb));
    }

    assert(!s.candidate_channels_.empty());
}