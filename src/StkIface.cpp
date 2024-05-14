#include "StkIface.h"
#include "TiogaBlock.h"
#include "Timer.h"

namespace tioga_nalu {

StkIface::StkIface(stk::ParallelMachine& comm)
    : comm_(comm)
    //, meta_()
    //, bulk_(meta_, comm, stk::mesh::BulkData::NO_AUTO_AURA)
    , meshBuilder_(comm)
    , stkio_(comm)
{
	meshBuilder_.set_aura_option(stk::mesh::BulkData::NO_AUTO_AURA);
	bulkData_=meshBuilder_.create();
}

void StkIface::load(const YAML::Node& node)
{
    if (!stk::parallel_machine_rank(comm_)) {
        std::cout << "Reading inputs for STK mesh" << std::endl;
    }
    //const int nproc = stk::parallel_machine_size(bulk_.parallel());
    const int nproc = stk::parallel_machine_size(bulk_data().parallel());

    if ((nproc > 1) && node["decomposition_method"]) {
        auto decomp_method = node["decomposition_method"].as<std::string>();
        stkio_.property_add(
            Ioss::Property("DECOMPOSITION_METHOD", decomp_method));
    }

    {
        auto timeMon = get_timer("StkIface::init_meta_data");
        const auto inp_mesh = node["input_mesh"].as<std::string>();
        if (!stk::parallel_machine_rank(comm_)) {
            std::cout << "Reading meta data for: " << inp_mesh << std::endl;
        }
        stkio_.add_mesh_database(inp_mesh, stk::io::READ_MESH);
        //stkio_.set_bulk_data(bulk_);
        stkio_.set_bulk_data(bulk_data());
        stkio_.create_input_mesh();
        stkio_.add_all_mesh_fields_as_input_fields();
    }

    if (node["motion_info"]) {
        has_motion_ = true;
        const auto& mnode = node["motion_info"];
        //motion_.reset(new MeshMotion(meta_, bulk_, mnode));
        motion_.reset(new MeshMotion(meta_data(), bulk_data(), mnode));
    }

    const auto& oset_info = node["overset_info"];
    //tg_.reset(new TiogaSTKIface(meta_, bulk_, oset_info, coordinates_name()));
    tg_.reset(new TiogaSTKIface(meta_data(), bulk_data(), oset_info, coordinates_name()));
}

void StkIface::setup()
{
    if (!stk::parallel_machine_rank(comm_)) {
        std::cout << "Performing STK setup" << std::endl;
    }
    auto timeMon = get_timer("StkIface::setup");
    if (has_motion_) motion_->setup();
    tg_->setup();

    if (num_vars() > 0) {
        //auto& qvar = meta_.declare_field<GenericFieldType>(
        //    stk::topology::NODE_RANK, "qvars");
        auto& qvar = meta_data().declare_field<GenericFieldType>(
            stk::topology::NODE_RANK, "qvars");
        //stk::mesh::put_field_on_mesh(
        //    qvar, meta_.universal_part(), num_vars(), nullptr);
        stk::mesh::put_field_on_mesh(
            qvar, meta_data().universal_part(), num_vars(), nullptr);
    }
}

void StkIface::initialize()
{
    if (!stk::parallel_machine_rank(comm_)) {
        std::cout << "Performing STK initialize" << std::endl;
    }
    auto timeMon = get_timer("StkIface::initialize");
    if (has_motion_) motion_->initialize();
    tg_->initialize();
}

void StkIface::populate_bulk_data()
{
    if (!stk::parallel_machine_rank(comm_)) {
        std::cout << "Reading bulk data" << std::endl;
    }
    auto timeMon = get_timer("StkIface::populate_bulk_data");
    stkio_.populate_bulk_data();
}

void StkIface::load_and_initialize_all(const YAML::Node& node)
{
    load(node);
    setup();
    populate_bulk_data();
    initialize();
}

void StkIface::register_solution()
{
    if (num_vars() < 1) return;

    init_vars();
    tg_->register_solution(num_vars());
}

size_t StkIface::write_outputs(const YAML::Node& node, const double time)
{
    auto tmon = tioga_nalu::get_timer("TiogaAMRIface::write_outputs");

    bool has_motion = false;
    if (node["motion_info"]) has_motion = true;

    //auto* cell_vol = meta_.get_field<ScalarFieldType>(
    //    stk::topology::ELEM_RANK, "cell_volume");
    auto* cell_vol = meta_data().get_field<ScalarFieldType>(
        stk::topology::ELEM_RANK, "cell_volume");
    //auto* nodal_vol = meta_.get_field<ScalarFieldType>(
    //    stk::topology::NODE_RANK, "nodal_volume");
    auto* nodal_vol = meta_data().get_field<ScalarFieldType>(
        stk::topology::NODE_RANK, "nodal_volume");
    //ScalarFieldType* ibf =
    //    meta_.get_field<ScalarFieldType>(stk::topology::NODE_RANK, "iblank");
    ScalarFieldType* ibf =
        meta_data().get_field<ScalarFieldType>(stk::topology::NODE_RANK, "iblank");
    //ScalarFieldType* ibcell = meta_.get_field<ScalarFieldType>(
    //    stk::topology::ELEM_RANK, "iblank_cell");
    ScalarFieldType* ibcell = meta_data().get_field<ScalarFieldType>(
        stk::topology::ELEM_RANK, "iblank_cell");

    std::string out_mesh = node["output_mesh"].as<std::string>();
    //if (bulk_.parallel_rank() == 0)
    if (bulk_data().parallel_rank() == 0)
        std::cout << "Writing STK output file: " << out_mesh << std::endl;
    size_t fh = stkio_.create_output_mesh(out_mesh, stk::io::WRITE_RESTART);
    stkio_.add_field(fh, *nodal_vol);
    stkio_.add_field(fh, *cell_vol);
    stkio_.add_field(fh, *ibf);
    stkio_.add_field(fh, *ibcell);

    if (has_motion) {
        //VectorFieldType* mesh_disp = meta_.get_field<VectorFieldType>(
        //    stk::topology::NODE_RANK, "mesh_displacement");
        VectorFieldType* mesh_disp = meta_data().get_field<VectorFieldType>(
            stk::topology::NODE_RANK, "mesh_displacement");
        stkio_.add_field(fh, *mesh_disp);
    }

    if (num_vars() > 0) {
        //auto* qvars = meta_.get_field<GenericFieldType>(
        //    stk::topology::NODE_RANK, "qvars");
        auto* qvars = meta_data().get_field<GenericFieldType>(
            stk::topology::NODE_RANK, "qvars");
        stkio_.add_field(fh, *qvars);
    }

    // Sync fields to host before output
    //for (auto* fld : meta_.get_fields()) fld->sync_to_host();
    for (auto* fld : meta_data().get_fields()) fld->sync_to_host();

    stkio_.begin_output_step(fh, time);
    stkio_.write_defined_output_fields(fh);
    stkio_.end_output_step(fh);

    return fh;
}

void StkIface::write_outputs(size_t fh, const double time)
{
    //if (bulk_.parallel_rank() == 0)
    if (bulk_data().parallel_rank() == 0)
        std::cout << "Writing to STK output file at: " << time << std::endl;

    // Sync fields to host before output
    //for (auto* fld : meta_.get_fields()) fld->sync_to_host();
    for (auto* fld : meta_data().get_fields()) fld->sync_to_host();

    stkio_.begin_output_step(fh, time);
    stkio_.write_defined_output_fields(fh);
    stkio_.end_output_step(fh);
}

void StkIface::init_vars()
{
    auto tmon = tioga_nalu::get_timer("TiogaAMRIface::init_vars");

    //auto* coords = meta_.get_field<VectorFieldType>(
    //    stk::topology::NODE_RANK, coordinates_name());
    auto* coords = meta_data().get_field<VectorFieldType>(
        stk::topology::NODE_RANK, coordinates_name());
    //auto* qvars =
    //    meta_.get_field<GenericFieldType>(stk::topology::NODE_RANK, "qvars");
    auto* qvars =
        meta_data().get_field<GenericFieldType>(stk::topology::NODE_RANK, "qvars");
    const stk::mesh::Selector sel = stk::mesh::selectField(*qvars);
    //const auto& bkts = bulk_.get_buckets(stk::topology::NODE_RANK, sel);
    const auto& bkts = bulk_data().get_buckets(stk::topology::NODE_RANK, sel);

    for (auto b : bkts) {
        for (size_t in = 0; in < b->size(); ++in) {
            const auto node = (*b)[in];
            const double* xyz = stk::mesh::field_data(*coords, node);
            double* qq = stk::mesh::field_data(*qvars, node);

            if (ncell_vars_ > 0) {
                for (int n = 0; n < ncell_vars_; ++n) {
                    const double xfac = 1.0 * ((n + 1) << n);
                    const double yfac = 1.0 * ((n + 2) << n);
                    const double zfac = 1.0 * ((n + 3) << n);
                    qq[n] = xfac * xyz[0] + yfac * xyz[1] + zfac * xyz[2];
                }
            }

            if (nnode_vars_ > 0) {
                const int ii = ncell_vars_;
                for (int n = 0; n < nnode_vars_; ++n) {
                    const double xfac = 1.0 * ((n + 1) << n);
                    const double yfac = 1.0 * ((n + 2) << n);
                    const double zfac = 1.0 * ((n + 3) << n);
                    qq[ii + n] = xfac * xyz[0] + yfac * xyz[1] + zfac * xyz[2];
                }
            }
        }
    }
}

bool StkIface::get_hole_map_algorithm()
{
	return tg_->get_hole_map_algorithm();
}

stk::mesh::BulkData&
StkIface::bulk_data()
{
return *bulkData_;
}

const stk::mesh::BulkData&
StkIface::bulk_data() const
{
return *bulkData_;
}

stk::mesh::MetaData&
StkIface::meta_data()
{
return bulkData_->mesh_meta_data();
}

const stk::mesh::MetaData&
StkIface::meta_data() const
{
return bulkData_->mesh_meta_data();
}

} // namespace tioga_nalu
