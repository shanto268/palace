// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include "curlcurloperator.hpp"

#include "utils/communication.hpp"
#include "utils/geodata.hpp"
#include "utils/iodata.hpp"
#include "utils/mfemcoefficients.hpp"
#include "utils/mfemintegrators.hpp"
#include "utils/mfemoperators.hpp"
#include "utils/prettyprint.hpp"

namespace palace
{

namespace
{

mfem::Array<int> SetUpBoundaryProperties(const IoData &iodata, const mfem::ParMesh &mesh)
{
  int bdr_attr_max = mesh.bdr_attributes.Max();
  if (!iodata.boundaries.pec.empty())
  {
    // Check that boundary attributes have been specified correctly.
    mfem::Array<int> bdr_attr_marker(bdr_attr_max);
    bdr_attr_marker = 0;
    for (auto attr : mesh.bdr_attributes)
    {
      bdr_attr_marker[attr - 1] = 1;
    }
    bool first = true;
    for (auto attr : iodata.boundaries.pec.attributes)
    {
      // MFEM_VERIFY(attr > 0 && attr <= bdr_attr_max,
      //             "PEC boundary attribute tags must be non-negative and correspond to "
      //             "attributes in the mesh!");
      // MFEM_VERIFY(bdr_attr_marker[attr-1],
      //             "Unknown PEC boundary attribute " << attr << "!");
      if (attr <= 0 || attr > bdr_attr_marker.Size() || !bdr_attr_marker[attr - 1])
      {
        if (first)
        {
          Mpi::Print("\n");
          first = false;
        }
        Mpi::Warning("Unknown PEC boundary attribute {:d}!\nSolver will just ignore it!\n",
                     attr);
      }
    }
  }

  // Mark selected boundary attributes from the mesh as essential (Dirichlet).
  mfem::Array<int> dbc_bcs, dbc_marker;
  dbc_bcs.Reserve(static_cast<int>(iodata.boundaries.pec.attributes.size()));
  for (auto attr : iodata.boundaries.pec.attributes)
  {
    if (attr <= 0 || attr > bdr_attr_max)
    {
      continue;  // Can just ignore if wrong
    }
    dbc_bcs.Append(attr);
  }
  mesh::AttrToMarker(bdr_attr_max, dbc_bcs, dbc_marker);
  return dbc_marker;
}

std::vector<std::unique_ptr<mfem::ND_FECollection>> ConstructFECollections(bool pc_gmg,
                                                                           int p, int dim)
{
  MFEM_VERIFY(p > 0, "Nedelec space order must be positive!");
  int b1 = mfem::BasisType::GaussLobatto, b2 = mfem::BasisType::GaussLegendre;
  std::vector<std::unique_ptr<mfem::ND_FECollection>> nd_fecs;
  if (pc_gmg)
  {
    nd_fecs.reserve(p);
    for (int o = 1; o <= p; o++)
    {
      nd_fecs.push_back(std::make_unique<mfem::ND_FECollection>(o, dim, b1, b2));
    }
  }
  else
  {
    nd_fecs.reserve(1);
    nd_fecs.push_back(std::make_unique<mfem::ND_FECollection>(p, dim, b1, b2));
  }
  return nd_fecs;
}

mfem::ParFiniteElementSpaceHierarchy ConstructFiniteElementSpaceHierarchy(
    std::vector<std::unique_ptr<mfem::ParMesh>> &mesh,
    const std::vector<std::unique_ptr<mfem::ND_FECollection>> &nd_fecs,
    const mfem::Array<int> &dbc_marker)
{
  MFEM_VERIFY(!mesh.empty() && !nd_fecs.empty(),
              "Empty mesh or FE collection for FE space construction!");
  auto *nd_fespace = new mfem::ParFiniteElementSpace(mesh[0].get(), nd_fecs[0].get());
  mfem::ParFiniteElementSpaceHierarchy nd_fespaces(mesh[0].get(), nd_fespace, false, true);
  // h-refinement
  for (std::size_t l = 1; l < mesh.size(); l++)
  {
    nd_fespace = new mfem::ParFiniteElementSpace(mesh[l].get(), nd_fecs[0].get());
    auto *P = new ZeroWrapTransferOperator(nd_fespaces.GetFinestFESpace(), *nd_fespace,
                                           dbc_marker);
    nd_fespaces.AddLevel(mesh[l].get(), nd_fespace, P, false, true, true);
  }
  // p-refinement
  for (std::size_t l = 1; l < nd_fecs.size(); l++)
  {
    nd_fespace = new mfem::ParFiniteElementSpace(mesh.back().get(), nd_fecs[l].get());
    auto *P = new ZeroWrapTransferOperator(nd_fespaces.GetFinestFESpace(), *nd_fespace,
                                           dbc_marker);
    nd_fespaces.AddLevel(mesh.back().get(), nd_fespace, P, false, true, true);
  }
  return nd_fespaces;
}

mfem::ParFiniteElementSpaceHierarchy
ConstructFiniteElementSpaceHierarchy(mfem::ParMesh &mesh,
                                     const mfem::ND_FECollection &nd_fec)
{
  auto *nd_fespace = new mfem::ParFiniteElementSpace(&mesh, &nd_fec);
  return mfem::ParFiniteElementSpaceHierarchy(&mesh, nd_fespace, false, true);
}

}  // namespace

CurlCurlOperator::CurlCurlOperator(const IoData &iodata,
                                   std::vector<std::unique_ptr<mfem::ParMesh>> &mesh)
  : dbc_marker(SetUpBoundaryProperties(iodata, *mesh.back())), skip_zeros(0),
    pc_gmg(iodata.solver.linear.mat_gmg), print_hdr(true),
    nd_fecs(ConstructFECollections(pc_gmg, iodata.solver.order, mesh.back()->Dimension())),
    h1_fec(iodata.solver.order, mesh.back()->Dimension()),
    rt_fec(iodata.solver.order - 1, mesh.back()->Dimension()),
    nd_fespaces(pc_gmg
                    ? ConstructFiniteElementSpaceHierarchy(mesh, nd_fecs, dbc_marker)
                    : ConstructFiniteElementSpaceHierarchy(*mesh.back(), *nd_fecs.back())),
    h1_fespace(mesh.back().get(), &h1_fec), rt_fespace(mesh.back().get(), &rt_fec),
    mat_op(iodata, *mesh.back()), surf_j_op(iodata, h1_fespace)
{
  // Finalize setup.
  CheckBoundaryProperties();
  nd_fespaces.GetFinestFESpace().GetEssentialTrueDofs(dbc_marker, dbc_tdof_list);

  // Print essential BC information.
  if (dbc_marker.Max() > 0)
  {
    Mpi::Print("\nConfiguring Dirichlet BC at attributes:\n");
    utils::PrettyPrintMarker(dbc_marker);
  }
}

void CurlCurlOperator::CheckBoundaryProperties()
{
  // A final check that no boundary attribute is assigned multiple boundary conditions.
  const auto &surf_j_marker = surf_j_op.GetMarker();
  for (int i = 0; i < dbc_marker.Size(); i++)
  {
    MFEM_VERIFY(dbc_marker[i] + surf_j_marker[i] <= 1,
                "Boundary attributes should not be specified with multiple BC!");
  }
}

void CurlCurlOperator::PrintHeader()
{
  if (print_hdr)
  {
    Mpi::Print("\nConfiguring system matrices, number of global unknowns: {:d}\n",
               nd_fespaces.GetFinestFESpace().GlobalTrueVSize());
    print_hdr = false;
  }
}

void CurlCurlOperator::GetStiffnessMatrix(std::vector<std::unique_ptr<mfem::Operator>> &K)
{
  K.clear();
  K.reserve(nd_fespaces.GetNumLevels());
  for (int l = 0; l < nd_fespaces.GetNumLevels(); l++)
  {
    auto &nd_fespace_l = nd_fespaces.GetFESpaceAtLevel(l);
    mfem::Array<int> dbc_tdof_list_l;
    nd_fespace_l.GetEssentialTrueDofs(dbc_marker, dbc_tdof_list_l);

    MaterialPropertyCoefficient<MaterialPropertyType::INV_PERMEABILITY> muinv_func(mat_op);
    mfem::ParBilinearForm k(&nd_fespace_l);
    k.AddDomainIntegrator(new mfem::CurlCurlIntegrator(muinv_func));
    // k.SetAssemblyLevel(mfem::AssemblyLevel::FULL);
    k.Assemble(skip_zeros);
    k.Finalize(skip_zeros);
    mfem::HypreParMatrix *hK = k.ParallelAssemble();
    hK->EliminateBC(dbc_tdof_list_l, mfem::Operator::DiagonalPolicy::DIAG_ONE);
    PrintHeader();
    {
      std::string str = "";
      if (pc_gmg)
      {
        str =
            fmt::format(" (Level {:d}, {:d} unknowns)", l, nd_fespace_l.GlobalTrueVSize());
      }
      Mpi::Print(" K{}: NNZ = {:d}, norm = {:e}\n", str, hK->NNZ(),
                 hypre_ParCSRMatrixFnorm(*hK));
    }
    K.emplace_back(hK);
  }
}

std::unique_ptr<mfem::Operator> CurlCurlOperator::GetCurlMatrix()
{
  mfem::ParDiscreteLinearOperator curl(&nd_fespaces.GetFinestFESpace(), &rt_fespace);
  curl.AddDomainInterpolator(new mfem::CurlInterpolator);
  // curl.SetAssemblyLevel(mfem::AssemblyLevel::FULL);
  curl.Assemble();
  curl.Finalize();
  return std::unique_ptr<mfem::HypreParMatrix>(curl.ParallelAssemble());
}

void CurlCurlOperator::GetExcitationVector(int idx, mfem::Vector &RHS)
{
  // Assemble the surface current excitation +J. The SurfaceCurrentOperator assembles -J
  // (meant for time or frequency domain Maxwell discretization, so we multiply by -1 to
  // retrieve +J).
  SumVectorCoefficient fb(nd_fespaces.GetFinestFESpace().GetParMesh()->SpaceDimension());
  surf_j_op.AddExcitationBdrCoefficients(idx, fb);
  RHS.SetSize(nd_fespaces.GetFinestFESpace().GetTrueVSize());
  RHS = 0.0;
  if (fb.empty())
  {
    return;
  }
  mfem::ParLinearForm rhs(&nd_fespaces.GetFinestFESpace());
  rhs.AddBoundaryIntegrator(new VectorFEBoundaryLFIntegrator(fb));
  rhs.UseFastAssembly(true);
  rhs.Assemble();
  rhs.ParallelAssemble(RHS);
  RHS.Neg();
  RHS.SetSubVector(dbc_tdof_list, 0.0);
}

}  // namespace palace
