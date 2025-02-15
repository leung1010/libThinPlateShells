#include <memory>
#include <iostream>
#include <iomanip>
#include <igl/writeOBJ.h>

#include "../Common/Timer.h"
#include "../Collision/Collision.h"
#include "../ExternalEnergies/PressureEnergy.h"
#include "../ExternalEnergies/PenaltyEnergy.h"
#include "../Collision/CTCD.h"

#include "ElasticShellModel.h"
#include "ElasticShellMaterial.h"
#include "StVKMaterial.h"
#include "StVKTensionFieldMaterial.h"
#include "NeoHookeanMaterial.h"
#include "ElasticEnergy.h"
#include "quadraticBendingEnergy.h"
#include "curveSmoothedHingeBendingEnergy.h"
#include "corotationalHingeBendingEnergy.h"
#include "corotationalCurveFvmHingeBendingEnergy.h"
#include "cubic_shell.h"
#include "corotationalFlatFvmHingeBendingEnergy.h"
#include "flatSmoothedHingeBendingEnergy.h"
#include "corotationalCurveHingeBendingEnergy.h"

Projection::Projection(const std::vector<bool>& keepDOFs)
{
	int fulldofs = keepDOFs.size();
	dofmap.resize(fulldofs);
	int idx = 0;
	for (int i = 0; i < fulldofs; i++)
	{
		if (keepDOFs[i])
		{
			dofmap[i] = idx;
			invdofmap.push_back(i);
			idx++;
		}
		else
			dofmap[i] = -1;
	}
}

void Projection::projectVector(const Eigen::VectorXd& fullVec, Eigen::VectorXd& projVec) const
{
	int projdofs = invdofmap.size();
	projVec.resize(projdofs);

	for (int i = 0; i < projdofs; i++)
	{
		projVec[i] = fullVec[invdofmap[i]];
	}
}

void Projection::unprojectVector(const Eigen::VectorXd& projVec, Eigen::VectorXd& fullVec) const
{
	int fulldofs = dofmap.size();
	fullVec.resize(fulldofs);
	for (int i = 0; i < fulldofs; i++)
	{
		fullVec[i] = (dofmap[i] == -1 ? 0.0 : projVec[dofmap[i]]);
	}
}

void Projection::projectMatrix(std::vector<Eigen::Triplet<double> >& mat) const
{	
	int dim = mat.size();
	for(int i=0; i<dim; i++)
	{
		int r = mat[i].row();
		int c = mat[i].col();
		int pr = dofmap[r];
		int pc = dofmap[c];
		if (pr != -1 && pc != -1)
		{
			mat[i] = { pr,pc,mat[i].value() };
		}
		else
		{
			mat[i] = { 0,0,0.0 };
		}
	}
}

bool ElasticShellModel::initialization(const ElasticSetup setup, const ElasticState initialGuess, std::string filePrefix, bool posHess, bool isParallel)
{
	_setup = setup;
	_state = initialGuess;
	_lameAlpha = setup.YoungsModulus * setup.PoissonsRatio / (1.0 - setup.PoissonsRatio * setup.PoissonsRatio);
	_lameBeta = setup.YoungsModulus / 2.0 / (1.0 + setup.PoissonsRatio);
	_filePrefix = filePrefix;
	_isUsePosHess = posHess;
	_isParallel = isParallel;

	if (_isParallel)
		std::cout << "Use TBB for parallel computing the energy" << std::endl;
	else
		std::cout << "Sequential computing the energy" << std::endl;

	std::cout << "SFF type: " << _setup.sffType << std::endl;
	if (_state.curEdgeDOFs.size() != _setup.sff->numExtraDOFs() * _state.mesh.nEdges())
	{
		std::wcout << "mismatched edge dofs, please check the loading file process." << std::endl;
		return false;
	}
	setProjM();

	std::cout << "material type: " << std::endl;
    std::cout << "stretching: " << setup.strecthingType << std::endl;
    // bending energy
    std::cout << "bending:"  << _setup.bendingType << std::endl;

    _isC2 = setup.strecthingType != "tensionField";
	std::cout << "pressure : " << setup.pressure << std::endl;
	std::cout << "gravity coefficient: " << setup.gravity.transpose() << std::endl;
	std::cout << "Collision penalty constant is: " << setup.penaltyK << std::endl;
	return true;
}

void ElasticShellModel::setProjM()
{
	int nverts = _state.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();

	std::vector<bool> keepDOFs(3 * nverts + nedgedofs * nedges);

	int row = 0;
	for (int i = 0; i < nverts; i++)
	{
		for (int j = 0; j < 3; j++)
		{
			keepDOFs[row] = (_setup.clampedDOFs.find(3 * i + j) == _setup.clampedDOFs.end());
			row++;
		}
	}

	for (int i = 0; i < nedges; i++)
	{
		for (int j = 0; j < nedgedofs; j++)
		{
			keepDOFs[row] = true;
			row++;
		}
	}
	_proj = Projection(keepDOFs);
}

void ElasticShellModel::convertCurState2Variables(const ElasticState curState, Eigen::VectorXd& x)
{
	int nverts = curState.curPos.rows();
	int nedges = _state.mesh.nEdges();

	int nedgedofs = _setup.sff->numExtraDOFs();
	int extraDOFs = nedgedofs * nedges;

	Eigen::VectorXd fullX(3 * nverts + extraDOFs);

	for (int i = 0; i < nverts; i++)
	{
		fullX.segment(3 * i, 3) = curState.curPos.row(i);
	}

	fullX.segment(3 * nverts, nedgedofs * nedges) = curState.curEdgeDOFs;
	_proj.projectVector(fullX, x);	// exclude the clamped DOFs
}

void ElasticShellModel::convertVariables2CurState(const Eigen::VectorXd x, ElasticState& curState)
{
	curState = _state;
	int nverts = curState.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();

	Eigen::VectorXd fullx;
	_proj.unprojectVector(x, fullx);
	
	for (int i = 0; i < nverts; i++)
	{
		curState.curPos.row(i) = fullx.segment(3 * i, 3);
	}

	// enforce constrained DOFs
	for (auto& it : _setup.clampedDOFs)
	{
		int vid = it.first / 3;
		int coord = it.first % 3;
		curState.curPos(vid, coord) = it.second;
	}


	curState.curEdgeDOFs = fullx.segment(3 * nverts, nedgedofs * nedges);
	
}

double ElasticShellModel::value(const Eigen::VectorXd& x)
{
	int nverts = _state.curPos.rows();
	convertVariables2CurState(x, _state); // add the clamped DOFs to the current state
	double energy = 0;

	std::shared_ptr<ElasticShellMaterial> mat;

	// stretching energy
	if (_setup.strecthingType == "NeoHookean")
	{
		mat = std::make_shared<NeoHookeanMaterial>();
	}
	else if (_setup.strecthingType == "tensionField")
	{
		mat = std::make_shared<StVKTensionFieldMaterial>();	
	}
	else
	{
		mat = std::make_shared<StVKMaterial>();	
	}
	energy = elasticStretchingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, *_setup.sff, *mat, NULL, NULL, false, _isParallel);

	// bending energy
	double bendE = 0;
	if (std::strcmp(_setup.bendingType.c_str(), "midEdgeShell") == 0)
	{
		bendE = elasticBendingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, _setup.bbars, *_setup.sff, *mat, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "QS") == 0)
	{
		bendE = quadraticBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "CS") == 0)
	{
		bendE = cubicShellBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "EP") == 0)
	{
		bendE = corotationalHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "ES") == 0)
	{
		bendE = corotationalCurveHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "FP") == 0)
	{
		bendE = corotationalFlatFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "FS") == 0)
	{
		bendE = corotationalCurveFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "SP") == 0)
	{
		bendE = flatSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "SS") == 0)
	{
		bendE = curveSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	
	energy += bendE;
	
	// pressure
	if (_setup.pressure > 0)
	{
		double pressureE = pressureEnergy(_state.mesh.faces(), _state.curPos, _setup.pressure, NULL, NULL, Eigen::Vector3d::Zero(), false, _isParallel);
		energy += pressureE;
	}

	// gravity
	double gravityPotential = 0;
	for (int i = 0; i < nverts; i++)
	{
		Eigen::Vector3d pos = _state.curPos.row(i).transpose();
		Eigen::Vector3d mg = _setup.vertArea[i] * _setup.thickness * _setup.density * _setup.gravity;
		gravityPotential += -mg.dot(pos);
	}
	energy += gravityPotential;

	// point force
	double pointForceE = 0;
	for (const auto& pair : _setup.pointForces)
	{
		int dofID = pair.first;
		int nodeID = dofID / 3;
		int coordID = dofID % 3;
		double forceValue = pair.second;
		double coordValue = _state.curPos(nodeID, coordID);
		pointForceE +=  -forceValue * coordValue;
	}
	energy += pointForceE;

	// penalty forces
	if (_setup.penaltyK > 0.0)
	{
		Eigen::VectorXd penaltydE;
		double penaltyEnergy = penaltyForce_VertexFace(_state.curPos, _setup, NULL, NULL, false);
		energy += penaltyEnergy;
	}

	return energy;
}

double ElasticShellModel::stretchingValue(const Eigen::VectorXd& x)
{
	int nverts = _state.curPos.rows();
	convertVariables2CurState(x, _state);
	double energy = 0;
	// stretching energy
	std::shared_ptr<ElasticShellMaterial> mat;

    // stretching energy
    if (_setup.strecthingType == "NeoHookean")
    {
        mat = std::make_shared<NeoHookeanMaterial>();
    }
    else if (_setup.strecthingType == "tensionField")
    {
        mat = std::make_shared<StVKTensionFieldMaterial>();
    }
    else
    {
        mat = std::make_shared<StVKMaterial>();
    }
	energy = elasticStretchingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, *_setup.sff, *mat, NULL, NULL, false, _isParallel);
	
	return energy;
}

double ElasticShellModel::bendingValue(const Eigen::VectorXd& x)
{
	double energy = 0;

	std::shared_ptr<ElasticShellMaterial> mat;

    // stretching energy
    if (_setup.strecthingType == "NeoHookean")
    {
        mat = std::make_shared<NeoHookeanMaterial>();
    }
    else if (_setup.strecthingType == "tensionField")
    {
        mat = std::make_shared<StVKTensionFieldMaterial>();
    }
    else
    {
        mat = std::make_shared<StVKMaterial>();
    }

	// bending energy
	double bendE = 0;
	if (std::strcmp(_setup.bendingType.c_str(), "midEdgeShell") == 0)
	{
		bendE = elasticBendingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, _setup.bbars, *_setup.sff, *mat, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "QS") == 0)
	{
		bendE = quadraticBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "CS") == 0)
	{
		bendE = cubicShellBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "EP") == 0)
	{
		bendE = corotationalHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "ES") == 0)
	{
		bendE = corotationalCurveHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "FP") == 0)
	{
		bendE = corotationalFlatFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "FS") == 0)
	{
		bendE = corotationalCurveFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "SP") == 0)
	{
		bendE = flatSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "SS") == 0)
	{
		bendE = curveSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, NULL, false, _isParallel);
	}
	
	energy += bendE;

	return energy;
}


double ElasticShellModel::penaltyValue(const Eigen::VectorXd& x)
{
	if (_setup.penaltyK > 0.0)
	{
		double penaltyEnergy = penaltyForce_VertexFace(_state.curPos, _setup, NULL, NULL, false);
		return penaltyEnergy;
	}
	else
	{
		return 0;
	}
}

void ElasticShellModel::gradient(const Eigen::VectorXd& x, Eigen::VectorXd& grad)
{
	int nverts = _state.curPos.rows();
	convertVariables2CurState(x, _state);
	double energy = 0;
	std::shared_ptr<ElasticShellMaterial> mat;

    // stretching energy
    if (_setup.strecthingType == "NeoHookean")
    {
        mat = std::make_shared<NeoHookeanMaterial>();
    }
    else if (_setup.strecthingType == "tensionField")
    {
        mat = std::make_shared<StVKTensionFieldMaterial>();
    }
    else
    {
        mat = std::make_shared<StVKMaterial>();
    }
	energy = elasticStretchingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, *_setup.sff, *mat, &grad, NULL, false, _isParallel);

	// bending energy
	Eigen::VectorXd gradB = Eigen::VectorXd::Zero(grad.size());
	double bendE = 0;
	if (std::strcmp(_setup.bendingType.c_str(), "midEdgeShell") == 0)
	{
		bendE = elasticBendingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, _setup.bbars, *_setup.sff, *mat, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "QS") == 0)
	{
		bendE = quadraticBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "CS") == 0)
	{
		bendE = cubicShellBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "EP") == 0)
	{
		bendE = corotationalHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "ES") == 0)
	{
		bendE = corotationalCurveHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "FP") == 0)
	{
		bendE = corotationalFlatFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "FS") == 0)
	{
		bendE = corotationalCurveFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "SP") == 0)
	{
		bendE = flatSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "SS") == 0)
	{
		bendE = curveSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	
	energy += bendE;
	grad += gradB;

	if (_setup.pressure > 0)
	{
		Eigen::VectorXd pressuredE;
		double pressureE = pressureEnergy(_state.mesh.faces(), _state.curPos, _setup.pressure, &pressuredE, NULL, Eigen::Vector3d::Zero(), false, _isParallel);
		energy += pressureE;

		grad.segment(0, 3 * nverts) += pressuredE;

	}

	// gravity
	double gravityPotential = 0;
	for (int i = 0; i < nverts; i++)
	{
		Eigen::Vector3d pos = _state.curPos.row(i).transpose();
		Eigen::Vector3d mg = _setup.vertArea[i] * _setup.thickness * _setup.density * _setup.gravity;
		gravityPotential += -mg.dot(pos);

		grad.segment<3>(3 * i) += -mg;
	}
	energy += gravityPotential;

	// point force
	double pointForceE = 0;
	for (const auto& pair : _setup.pointForces)
	{
		int dofID = pair.first;
		int nodeID = dofID / 3;
		int coordID = dofID % 3;
		double forceValue = pair.second;
		pointForceE +=   -forceValue * _state.curPos(nodeID, coordID);
		grad(dofID) +=   -forceValue;
	}
	energy += pointForceE;

	// penalty forces
	if (_setup.penaltyK > 0.0)
	{
		Eigen::VectorXd penaltydE;
		double penaltyEnergy = penaltyForce_VertexFace(_state.curPos, _setup, &penaltydE, NULL, false);
		grad.segment(0, 3 * nverts) += penaltydE;
		energy += penaltyEnergy;
	}

	Eigen::VectorXd projgrad;
	_proj.projectVector(grad, projgrad);
	grad = projgrad;
}

Eigen::VectorXd ElasticShellModel::membraneGrad(const Eigen::VectorXd& x)
{
	int nverts = _state.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();
	int extraDOFs = nedgedofs * nedges;
    int dofs = 3 * nverts + extraDOFs;
	convertVariables2CurState(x, _state);
	double energy = 0;
	Eigen::VectorXd grad;
	grad = Eigen::VectorXd::Zero(dofs);
	std::shared_ptr<ElasticShellMaterial> mat;

    // stretching energy
    if (_setup.strecthingType == "NeoHookean")
    {
        mat = std::make_shared<NeoHookeanMaterial>();
    }
    else if (_setup.strecthingType == "tensionField")
    {
        mat = std::make_shared<StVKTensionFieldMaterial>();
    }
    else
    {
        mat = std::make_shared<StVKMaterial>();
    }
	energy = elasticStretchingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, *_setup.sff, *mat, &grad, NULL, false, _isParallel);
	
	Eigen::VectorXd projgrad;
	_proj.projectVector(grad, projgrad);
	grad = projgrad;

	return grad;
}


Eigen::VectorXd ElasticShellModel::bendingGrad(const Eigen::VectorXd& x)
{
	int nverts = _state.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();
	int extraDOFs = nedgedofs * nedges;
    int dofs = 3 * nverts + extraDOFs;
	convertVariables2CurState(x, _state);
	double energy = 0;
	Eigen::VectorXd grad;
	grad = Eigen::VectorXd::Zero(dofs);
	std::shared_ptr<ElasticShellMaterial> mat;

    // stretching energy
    if (_setup.strecthingType == "NeoHookean")
    {
        mat = std::make_shared<NeoHookeanMaterial>();
    }
    else if (_setup.strecthingType == "tensionField")
    {
        mat = std::make_shared<StVKTensionFieldMaterial>();
    }
    else
    {
        mat = std::make_shared<StVKMaterial>();
    }

	// bending energy
	Eigen::VectorXd gradB;
	double bendE = 0;
	if (std::strcmp(_setup.bendingType.c_str(), "midEdgeShell") == 0)
	{
		bendE = elasticBendingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, _setup.bbars, *_setup.sff, *mat, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "QS") == 0)
	{
		bendE = quadraticBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "CS") == 0)
	{
		bendE = cubicShellBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "EP") == 0)
	{
		bendE = corotationalHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "ES") == 0)
	{
		bendE = corotationalCurveHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "FP") == 0)
	{
		bendE = corotationalFlatFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "FS") == 0)
	{
		bendE = corotationalCurveFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "SP") == 0)
	{
		bendE = flatSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "SS") == 0)
	{
		bendE = curveSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, &gradB, NULL, false, _isParallel);
	}
	
	energy += bendE;
	grad += gradB;

	Eigen::VectorXd projgrad;
	_proj.projectVector(grad, projgrad);
	grad = projgrad;

	return grad;
}

void ElasticShellModel::externalForces(const Eigen::VectorXd& x, Eigen::VectorXd& grad)
{
	int nverts = _state.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();
	int extraDOFs = nedgedofs * nedges;
    int dofs = 3 * nverts + extraDOFs;
	convertVariables2CurState(x, _state);

	grad = Eigen::VectorXd::Zero(dofs); 
	// pressure
	if (_setup.pressure > 0)
	{
		Eigen::VectorXd pressuredE;
		double pressureE = pressureEnergy(_state.mesh.faces(), _state.curPos, _setup.pressure, &pressuredE, NULL, Eigen::Vector3d::Zero(), false, _isParallel);
		grad.segment(0, 3 * nverts) += pressuredE;
	}

	// gravity
	for (int i = 0; i < nverts; i++)
	{
		Eigen::Vector3d pos = _state.curPos.row(i).transpose();
		Eigen::Vector3d mg = _setup.vertArea[i] * _setup.thickness * _setup.density * _setup.gravity;
		grad.segment<3>(3 * i) += -mg;
	}
	
	// point force
	for (const auto& pair : _setup.pointForces)
	{
		int dofID = pair.first;
		int nodeID = dofID / 3;
		int coordID = dofID % 3;
		double forceValue = pair.second;
		grad(dofID) +=   -forceValue;
	}

	// penalty forces
	if (_setup.penaltyK > 0.0)
	{
		Eigen::VectorXd penaltydE;
		double penaltyEnergy = penaltyForce_VertexFace(_state.curPos, _setup, &penaltydE, NULL, false);
		grad.segment(0, 3 * nverts) += penaltydE;
	}

	Eigen::VectorXd projgrad;
	_proj.projectVector(grad, projgrad);
	grad = projgrad;
}


Eigen::VectorXd ElasticShellModel::externalForces(const Eigen::VectorXd& x)
{
	int nverts = _state.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();
	int extraDOFs = nedgedofs * nedges;
    int dofs = 3 * nverts + extraDOFs;
	convertVariables2CurState(x, _state);

	Eigen::VectorXd grad;
	grad = Eigen::VectorXd::Zero(dofs); 
	// pressure
	if (_setup.pressure > 0)
	{
		Eigen::VectorXd pressuredE;
		double pressureE = pressureEnergy(_state.mesh.faces(), _state.curPos, _setup.pressure, &pressuredE, NULL, Eigen::Vector3d::Zero(), false, _isParallel);
		grad.segment(0, 3 * nverts) += pressuredE;
	}

	// gravity
	for (int i = 0; i < nverts; i++)
	{
		Eigen::Vector3d pos = _state.curPos.row(i).transpose();
		Eigen::Vector3d mg = _setup.vertArea[i] * _setup.thickness * _setup.density * _setup.gravity;
		grad.segment<3>(3 * i) += -mg;
	}
	
	// point force
	for (const auto& pair : _setup.pointForces)
	{
		int dofID = pair.first;
		int nodeID = dofID / 3;
		int coordID = dofID % 3;
		double forceValue = pair.second;
		grad(dofID) +=   -forceValue;
	}

	// penalty forces
	if (_setup.penaltyK > 0.0)
	{
		Eigen::VectorXd penaltydE;
		double penaltyEnergy = penaltyForce_VertexFace(_state.curPos, _setup, &penaltydE, NULL, false);
		grad.segment(0, 3 * nverts) += penaltydE;
	}

	Eigen::VectorXd projgrad;
	_proj.projectVector(grad, projgrad);
	grad = projgrad;

	return grad;
}

void ElasticShellModel::hessian(const Eigen::VectorXd& x, Eigen::SparseMatrix<double>& hessian)
{
	Timer timer;
	std::vector<Eigen::Triplet<double> > hessianT;

	convertVariables2CurState(x, _state);
	double energy = 0;
	std::shared_ptr<ElasticShellMaterial> mat;

	int nverts = _state.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();

    if (_setup.strecthingType == "NeoHookean")
    {
        mat = std::make_shared<NeoHookeanMaterial>();
    }

    else if (_setup.strecthingType == "tensionField")
    {
        mat = std::make_shared<StVKTensionFieldMaterial>();
    }
    else
    {
        mat = std::make_shared<StVKMaterial>();
    }

	// stretching energy
	timer.start();
	energy = elasticStretchingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, *_setup.sff, *mat, NULL, &hessianT, _isUsePosHess, _isParallel);
	timer.stop();
	std::cout << "elastic stretching hessian took: " << timer.elapsedSeconds() << std::endl;

	// bending energy
	timer.start();
	std::vector<Eigen::Triplet<double> > bendingHcoeffs;
	bendingHcoeffs.clear();
	double bendE = 0;
	if (std::strcmp(_setup.bendingType.c_str(), "midEdgeShell") == 0)
	{
		bendE = elasticBendingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, _setup.bbars, *_setup.sff, *mat, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "QS") == 0)
	{
		bendE = quadraticBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "CS") == 0)
	{
		bendE = cubicShellBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "EP") == 0)
	{
		bendE = corotationalHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "ES") == 0)
	{
		bendE = corotationalCurveHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	} 
	else if (std::strcmp(_setup.bendingType.c_str(), "FP") == 0)
	{
		bendE = corotationalFlatFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	} 
	else if (std::strcmp(_setup.bendingType.c_str(), "FS") == 0)
	{
		bendE = corotationalCurveFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	} 
	else if (std::strcmp(_setup.bendingType.c_str(), "SP") == 0)
	{
		bendE = flatSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	} 
	else if (std::strcmp(_setup.bendingType.c_str(), "SS") == 0)
	{
		bendE = curveSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	
	timer.stop();
	std::cout << "bending hessian took: " << timer.elapsedSeconds() << std::endl;

	timer.start();
	energy += bendE;
	hessianT.insert(hessianT.end(), bendingHcoeffs.begin(), bendingHcoeffs.end());
	timer.stop();

	std::cout << "appending bending hessian took: " << timer.elapsedSeconds() << std::endl;
	timer.start();

	// pressure
	if (_setup.pressure > 0)
	{
		std::vector<Eigen::Triplet<double> > pressureHcoeffs;
		double pressureE = pressureEnergy(_state.mesh.faces(), _state.curPos, _setup.pressure, NULL, &pressureHcoeffs, Eigen::Vector3d::Zero(), false, _isParallel); // never use local PD-projection for pressure, since it is always indefinite.
 		timer.stop();
		std::cout << "pressure hessian took: " << timer.elapsedSeconds() << std::endl;

		timer.start();
		energy += pressureE;
		hessianT.insert(hessianT.end(), pressureHcoeffs.begin(), pressureHcoeffs.end());
		timer.stop();

		std::cout << "appending pressure hessian took: " << timer.elapsedSeconds() << std::endl;
		timer.start();
	}
	
	// gravity
	timer.start();
	double gravityPotential = 0;
	for (int i = 0; i < nverts; i++)
	{
		Eigen::Vector3d pos = _state.curPos.row(i).transpose();
		Eigen::Vector3d mg = _setup.vertArea[i] * _setup.thickness * _setup.density * _setup.gravity;
		gravityPotential += -mg.dot(pos);
	}
	energy += gravityPotential;
	timer.stop();
	std::cout<<"gravity hessian took: "<<timer.elapsedSeconds()<<std::endl;

	// point force
	timer.start();
	double pointForceE = 0;
	for (const auto& pair : _setup.pointForces)
	{
		int dofID = pair.first;
		int nodeID = dofID / 3;
		int coordID = dofID % 3;
		double forceValue = pair.second;
		pointForceE +=   -forceValue * _state.curPos(nodeID, coordID);
	}
	energy += pointForceE;
	timer.stop();
	std::cout<<"point force hessian took: "<<timer.elapsedSeconds()<<std::endl;

	// penalty forces
	timer.start();
	if (_setup.penaltyK > 0.0)
	{
		std::vector<Eigen::Triplet<double> > penaltyHcoeffs;
		double penaltyEnergy = penaltyForce_VertexFace(_state.curPos, _setup, NULL, &penaltyHcoeffs, _isUsePosHess);
		energy += penaltyEnergy;
		hessianT.insert(hessianT.end(), penaltyHcoeffs.begin(), penaltyHcoeffs.end());
	}
	timer.stop();
	std::cout<<"penalty hessian took: "<<timer.elapsedSeconds()<<std::endl;

	timer.start();
	std::vector<Eigen::Triplet<double> > projHcoeffs;
	_proj.projectMatrix(hessianT);
	timer.stop();
	std::cout << "projecting hessian took: " << timer.elapsedSeconds() << std::endl;

	timer.start();
	hessian.resize(_proj.projDOFs(), _proj.projDOFs());
	hessian.setFromTriplets(hessianT.begin(), hessianT.end());
	timer.stop();
	std::cout << "setting hessian from triplet took: " << timer.elapsedSeconds() << std::endl;	
}

Eigen::SparseMatrix<double> ElasticShellModel::membraneHessian(const Eigen::VectorXd& x)
{
	Timer timer;
	Eigen::SparseMatrix<double> hessian;
	std::vector<Eigen::Triplet<double> > hessianT;

	convertVariables2CurState(x, _state);
	double energy = 0;
	std::shared_ptr<ElasticShellMaterial> mat;

	int nverts = _state.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();

    if (_setup.strecthingType == "NeoHookean")
    {
        mat = std::make_shared<NeoHookeanMaterial>();
    }

    else if (_setup.strecthingType == "tensionField")
    {
        mat = std::make_shared<StVKTensionFieldMaterial>();
    }
    else
    {
        mat = std::make_shared<StVKMaterial>();
    }

	// stretching energy
	timer.start();
	energy = elasticStretchingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, *_setup.sff, *mat, NULL, &hessianT, _isUsePosHess, _isParallel);
	timer.stop();
	std::cout << "membrane hessian took: " << timer.elapsedSeconds() << std::endl;

	timer.start();
	std::vector<Eigen::Triplet<double> > projHcoeffs;
	_proj.projectMatrix(hessianT);
	timer.stop();
	std::cout << "projecting membrane hessian took: " << timer.elapsedSeconds() << std::endl;

	timer.start();
	hessian.resize(_proj.projDOFs(), _proj.projDOFs());
	hessian.setFromTriplets(hessianT.begin(), hessianT.end());
	timer.stop();
	std::cout << "setting membrane hessian from triplet took: " << timer.elapsedSeconds() << std::endl;	

	return hessian;
}

Eigen::SparseMatrix<double> ElasticShellModel::bendingHessian(const Eigen::VectorXd& x)
{
	Timer timer;
	Eigen::SparseMatrix<double> hessian;
	std::vector<Eigen::Triplet<double> > hessianT;

	convertVariables2CurState(x, _state);
	double energy = 0;
	std::shared_ptr<ElasticShellMaterial> mat;

	int nverts = _state.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();

    if (_setup.strecthingType == "NeoHookean")
    {
        mat = std::make_shared<NeoHookeanMaterial>();
    }

    else if (_setup.strecthingType == "tensionField")
    {
        mat = std::make_shared<StVKTensionFieldMaterial>();
    }
    else
    {
        mat = std::make_shared<StVKMaterial>();
    }

	// bending energy
	timer.start();
	std::vector<Eigen::Triplet<double> > bendingHcoeffs;
	bendingHcoeffs.clear();
	double bendE = 0;
	if (std::strcmp(_setup.bendingType.c_str(), "midEdgeShell") == 0)
	{
		bendE = elasticBendingEnergy(_state.mesh, _state.curPos, _state.curEdgeDOFs, _lameAlpha, _lameBeta, _setup.thickness, _setup.abars, _setup.bbars, *_setup.sff, *mat, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "QS") == 0)
	{
		bendE = quadraticBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "CS") == 0)
	{
		bendE = cubicShellBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "EP") == 0)
	{
		bendE = corotationalHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	else if (std::strcmp(_setup.bendingType.c_str(), "ES") == 0)
	{
		bendE = corotationalCurveHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	} 
	else if (std::strcmp(_setup.bendingType.c_str(), "FP") == 0)
	{
		bendE = corotationalFlatFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	} 
	else if (std::strcmp(_setup.bendingType.c_str(), "FS") == 0)
	{
		bendE = corotationalCurveFvmHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	} 
	else if (std::strcmp(_setup.bendingType.c_str(), "SP") == 0)
	{
		bendE = flatSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	} 
	else if (std::strcmp(_setup.bendingType.c_str(), "SS") == 0)
	{
		bendE = curveSmoothedHingeBendingEnergy(_state.mesh, _state.initialGuess, _state.curPos, _setup.YoungsModulus, _setup.PoissonsRatio, _setup.thickness, _setup.abars, *_setup.sff, NULL, &bendingHcoeffs, _isUsePosHess, _isParallel);
	}
	
	timer.stop();
	std::cout << "bending hessian took: " << timer.elapsedSeconds() << std::endl;

	timer.start();
	energy += bendE;
	hessianT.insert(hessianT.end(), bendingHcoeffs.begin(), bendingHcoeffs.end());
	timer.stop();
	std::cout << "appending bending hessian took: " << timer.elapsedSeconds() << std::endl;
	timer.start();

	timer.start();
	std::vector<Eigen::Triplet<double> > projHcoeffs;
	_proj.projectMatrix(hessianT);
	timer.stop();
	std::cout << "projecting bending hessian took: " << timer.elapsedSeconds() << std::endl;

	timer.start();
	hessian.resize(_proj.projDOFs(), _proj.projDOFs());
	hessian.setFromTriplets(hessianT.begin(), hessianT.end());
	timer.stop();
	std::cout << "setting bending hessian from triplet took: " << timer.elapsedSeconds() << std::endl;	

	return hessian;
}

Eigen::SparseMatrix<double> ElasticShellModel::exterHessian(const Eigen::VectorXd& x)
{
	Timer timer;
	Eigen::SparseMatrix<double> hessian;
	std::vector<Eigen::Triplet<double> > hessianT;

	convertVariables2CurState(x, _state);
	double energy = 0;

	int nverts = _state.curPos.rows();
	int nedges = _state.mesh.nEdges();
	int nedgedofs = _setup.sff->numExtraDOFs();

	// pressure
	if (_setup.pressure > 0)
	{
		std::vector<Eigen::Triplet<double> > pressureHcoeffs;
		double pressureE = pressureEnergy(_state.mesh.faces(), _state.curPos, _setup.pressure, NULL, &pressureHcoeffs, Eigen::Vector3d::Zero(), false, _isParallel); // never use local PD-projection for pressure, since it is always indefinite.
 		timer.stop();
		std::cout << "pressure hessian took: " << timer.elapsedSeconds() << std::endl;

		timer.start();
		energy += pressureE;
		hessianT.insert(hessianT.end(), pressureHcoeffs.begin(), pressureHcoeffs.end());
		timer.stop();

		std::cout << "appending pressure hessian took: " << timer.elapsedSeconds() << std::endl;
		timer.start();
	}
	
	// gravity
	timer.start();
	double gravityPotential = 0;
	for (int i = 0; i < nverts; i++)
	{
		Eigen::Vector3d pos = _state.curPos.row(i).transpose();
		Eigen::Vector3d mg = _setup.vertArea[i] * _setup.thickness * _setup.density * _setup.gravity;
		gravityPotential += -mg.dot(pos);
	}
	energy += gravityPotential;
	timer.stop();
	std::cout<<"gravity hessian took: "<<timer.elapsedSeconds()<<std::endl;

	// point force
	timer.start();
	double pointForceE = 0;
	for (const auto& pair : _setup.pointForces)
	{
		int dofID = pair.first;
		int nodeID = dofID / 3;
		int coordID = dofID % 3;
		double forceValue = pair.second;
		pointForceE +=   -forceValue * _state.curPos(nodeID, coordID);
	}
	energy += pointForceE;
	timer.stop();
	std::cout<<"point force hessian took: "<<timer.elapsedSeconds()<<std::endl;

	// penalty forces
	timer.start();
	if (_setup.penaltyK > 0.0)
	{
		std::vector<Eigen::Triplet<double> > penaltyHcoeffs;
		double penaltyEnergy = penaltyForce_VertexFace(_state.curPos, _setup, NULL, &penaltyHcoeffs, _isUsePosHess);
		energy += penaltyEnergy;
		hessianT.insert(hessianT.end(), penaltyHcoeffs.begin(), penaltyHcoeffs.end());
	}
	timer.stop();
	std::cout<<"penalty hessian took: "<<timer.elapsedSeconds()<<std::endl;

	timer.start();
	std::vector<Eigen::Triplet<double> > projHcoeffs;
	_proj.projectMatrix(hessianT);
	timer.stop();
	std::cout << "projecting external force hessian took: " << timer.elapsedSeconds() << std::endl;

	timer.start();
	hessian.resize(_proj.projDOFs(), _proj.projDOFs());
	hessian.setFromTriplets(hessianT.begin(), hessianT.end());
	timer.stop();
	std::cout << "setting external force hessian from triplet took: " << timer.elapsedSeconds() << std::endl;	

	return hessian;
}

double ElasticShellModel::getMaxStep(const Eigen::VectorXd& x, const Eigen::VectorXd& dir, double step)
{
	if (_setup.penaltyK > 0)
	{
		Eigen::VectorXd start = x;
		Eigen::VectorXd updated = start + dir * step;

		ElasticState startState, updatedState;
		convertVariables2CurState(start, startState);
		convertVariables2CurState(updated, updatedState);

		Eigen::MatrixXd startV = startState.curPos;
		Eigen::MatrixXd updatedV = updatedState.curPos;

		//collision detection
		std::vector<std::unique_ptr<AABB> > AABBs;
		for (int i = 0; i < _setup.obs.size(); i++)
			AABBs.emplace_back(buildAABB(_setup.obs[i].V, _setup.obs[i].V, _setup.obs[i].F, 0));

		int nverts = startV.rows();

		double scale = 1;
		bool isCollision = false;
		for (int i = 0; i < nverts; i++)
		{
			BoundingBox sweptLine;
			for (int j = 0; j < 3; j++)
			{
				sweptLine.mins[j] = std::min(startV(i, j), updatedV(i, j));
				sweptLine.maxs[j] = std::max(startV(i, j), updatedV(i, j));
			}
			for (int j = 0; j < _setup.obs.size(); j++)
			{
				if (!AABBs[j]) // obstacle with zero faces, for some reason
					continue;

				std::vector<int> hits;
				AABBs[j]->intersect(sweptLine, hits);
				for (int k = 0; k < hits.size(); k++)
				{
					int face = hits[k];
					Eigen::RowVector3i obs_face = _setup.obs[j].F.row(face);
					Eigen::Vector3d q1start = _setup.obs[j].V.row(obs_face(0)).transpose();
					Eigen::Vector3d q2start = _setup.obs[j].V.row(obs_face(1)).transpose();
					Eigen::Vector3d q3start = _setup.obs[j].V.row(obs_face(2)).transpose();
					Eigen::Vector3d q1end = _setup.obs[j].V.row(obs_face(0)).transpose();
					Eigen::Vector3d q2end = _setup.obs[j].V.row(obs_face(1)).transpose();
					Eigen::Vector3d q3end = _setup.obs[j].V.row(obs_face(2)).transpose();
					Eigen::Vector3d q0start = startV.row(i).transpose();
					Eigen::Vector3d q0end = updatedV.row(i).transpose();
					double t = 1;
					bool local_collision = CTCD::vertexFaceCTCD(q0start, q1start, q2start, q3start, q0end, q1end, q2end, q3end, 1e-5, t);
					if (local_collision && (t < scale))
					{
						isCollision = true;
						scale = t;
						/*if (scale < 1e-5)
						{
							std::cout << scale << std::endl;
							std::cout << "vertex " << i << ": " << q0start.transpose() << std::endl;
							std::cout << "end point " << q0end.transpose() << std::endl;
							std::cout << "face : " << face << std::endl;
							std::cout << q1start.transpose() << std::endl;
							std::cout << q2start.transpose() << std::endl;
							std::cout << q3start.transpose() << std::endl;
							std::cout << "full dir : " << fullDir.segment(3 * i, 3).transpose() << std::endl;
							std::cout << "full grad : " << fullgrad.segment(3 * i, 3).transpose() << std::endl;
						}*/
						
					}
				}
			}
		}
		if (isCollision)
			return (scale * step * 0.95);
		else
			return scale * step;
	}

	else
	{
		return _setup.maxStepSize > 0 ? _setup.maxStepSize : 1.0;
	}

}

Eigen::SparseMatrix<double> ElasticShellModel::buildLinearConstraints()
{
	Eigen::SparseMatrix<double> A;
	return A;
}

void ElasticShellModel::save(int curIterations, TimeCost curTimeCost, double stepsize, double oldEnergy, double curEnergy, double gradnorm, double dirnorm, double reg, bool PSDHess)
{
	std::string statusFileName = _filePrefix + std::string("_elastic_status.txt");
	std::ofstream sfs;
	sfs.open(statusFileName, std::ofstream::out | std::ofstream::app);
	if (sfs && curIterations > 0)
	{
		sfs << "iter: " << curIterations << ", total time: " << curTimeCost.totalTime() << std::endl;
		sfs << "line search rate: " << stepsize << std::setprecision(std::numeric_limits<long double>::digits10 + 1) << ", actual hessian: ";
		if (PSDHess)
			sfs << "false";
		else
			sfs << "true";
		sfs << ", reg: " << reg << std::endl;
		sfs	<< "f_old: " << oldEnergy << ", f_new: " << curEnergy << ", delta_f : " << oldEnergy - curEnergy << ", ||grad||_inf : " << gradnorm << ", ||dir||_inf : " << dirnorm << std::endl;
	}

	std::string timingFileName = _filePrefix + std::string("_elastic_timing.txt");
	std::ofstream tfs;
	tfs.open(timingFileName, std::ofstream::out | std::ofstream::app);
	if (tfs && curIterations > 0)
	{
		tfs << "iter: " << curIterations << ", total time: " << curTimeCost.totalTime() << std::endl;
		tfs << "grad_cost: " << curTimeCost.gradTime << ", hess_cost: " << curTimeCost.hessTime << ", solver_cost: " << curTimeCost.solverTime << ", collision_cost: " << curTimeCost.collisionDectionTime << ", linesearch_cost: " << curTimeCost.lineSearchTime << ", update_cost: " << curTimeCost.updateTime << ", checkConverg_cost: " << curTimeCost.convergenceCheckTime << std::endl;
	}

	std::string filePathPrefix = _filePrefix + "_convergence/";
	std::string convergeFileName = filePathPrefix + std::string("elastic_energy.txt");
	std::ofstream efs;
	efs.open(convergeFileName, std::ofstream::out | std::ofstream::app);
	if (efs)
	{
		efs << std::setprecision(std::numeric_limits<long double>::digits10 + 1) << curEnergy << std::endl;
	}

	if (curIterations >= 0)
	{
		std::stringstream Filename;
		Filename << filePathPrefix << "/stvk/stvk_" << curIterations << ".obj";
		igl::writeOBJ(Filename.str(), _state.curPos, _state.mesh.faces());
	}
}

void ElasticShellModel::testValueAndGradient(const Eigen::VectorXd& x)
{
	std::cout << "Test value and gradient. " << std::endl;
	double f = value(x);
	Eigen::VectorXd grad;
	gradient(x, grad);
	Eigen::VectorXd dir = Eigen::VectorXd::Random(x.size());
	dir.normalize();
	for (int i = 3; i < 10; i++)
	{
		double eps = std::pow(10, -i);
		Eigen::VectorXd x1 = x + eps * dir;
		Eigen::VectorXd x2 = x - eps * dir;
		double f1 = value(x1);
		double f2 = value(x2);
		std::cout << std::endl << "eps: " << eps << std::endl;
		std::cout << std::setprecision(std::numeric_limits<long double>::digits10 + 1) << "energy: " << f << ", energy after perturbation (right, left): " << f1 << ", " << f2 << std::endl;
		std::cout << std::setprecision(6);
		std::cout << "right finite difference: " << (f1 - f) / eps << std::endl;
		std::cout << "left finite difference: " << (f - f2) / eps << std::endl;
		std::cout << "central difference: " << (f1 - f2) / 2 / eps << std::endl;
		std::cout << "direction derivative: " << grad.dot(dir) << std::endl;
		std::cout << "right error: " << std::abs((f1 - f) / eps - grad.dot(dir)) << ", left error: " << std::abs((f - f2) / eps - grad.dot(dir)) << ", central error: " << std::abs((f1 - f2) / 2 / eps - grad.dot(dir)) << std::endl;
	}
}

void ElasticShellModel::testGradientAndHessian(const Eigen::VectorXd& x)
{
	std::cout << "Test gradient and hessian. " << std::endl;
	Eigen::SparseMatrix<double> hess;
	Eigen::VectorXd deriv;

	auto posHessBackup = _isUsePosHess;

	_isUsePosHess = false;
	gradient(x, deriv);
	hessian(x, hess);
	
	Eigen::VectorXd dir = Eigen::VectorXd::Random(x.size());
	dir.normalize();
	for (int i = 3; i < 10; i++)
	{
		double eps = std::pow(10, -i);
		Eigen::VectorXd x1 = x + eps * dir;
		Eigen::VectorXd deriv1;
		gradient(x1, deriv1);

		std::cout << std::endl << "eps: " << eps << std::endl;
		std::cout << std::setprecision(6);
		std::cout << "finite difference: " << (deriv1 - deriv).norm() / eps << std::endl;
		std::cout << "direction derivative: " << (hess * dir).norm() << std::endl;
		std::cout << "error: " << ((deriv1 - deriv) / eps - hess * dir).norm() << std::endl;
	}
	_isUsePosHess = posHessBackup;
}

void ElasticShellModel::testMaxStep()
{
    Eigen::VectorXd dir;
    Eigen::VectorXd temp(12);
    temp.setZero();
    temp(2) = -1;
    temp(5) = -2;
    dir = temp;

    Eigen::VectorXd x;
    convertCurState2Variables(_state, x);

    for (int i = 0 ; i < 3; i ++)
    {
        double step = pow(10,i);
        double maxStep = getMaxStep(x, dir, step);
        std::cout << "initial step : " << step << " max step : " << maxStep << std::endl;
    }
}
