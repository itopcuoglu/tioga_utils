
#include "MotionBase.h"

#include "stk_mesh/base/Part.hpp"

namespace tioga_nalu {

void MotionBase::setup()
{
    for (auto pName : partNames_) {
        stk::mesh::Part* part = meta_.get_part(pName);
        if (nullptr == part)
            throw std::runtime_error(
                "MeshMotion: Invalid part name encountered: " + pName);
        else
            partVec_.push_back(part);
    }

    VectorFieldType& coordinates =
        meta_.declare_field<double>(stk::topology::NODE_RANK, "coordinates");
    VectorFieldType& current_coordinates = meta_.declare_field<double>(
        stk::topology::NODE_RANK, "current_coordinates");
    VectorFieldType& mesh_displacement = meta_.declare_field<double>(
        stk::topology::NODE_RANK, "mesh_displacement");
    int fieldSize = 3;

    for (auto* p : partVec_) {
        stk::mesh::put_field_on_mesh(coordinates, *p, fieldSize, nullptr);
        stk::mesh::put_field_on_mesh(
            current_coordinates, *p, fieldSize, nullptr);
        stk::mesh::put_field_on_mesh(mesh_displacement, *p, fieldSize, nullptr);
    }
}

} // namespace tioga_nalu
