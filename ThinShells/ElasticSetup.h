#pragma once

#include <set>
#include <vector>
#include <map>
#include <Eigen/Sparse>
#include <memory>

#include "../MeshLib/MeshConnectivity.h"
#include "../MeshLib/MeshGeometry.h"
#include "../Collision/Obstacle.h"
#include "../SecondFundamentalForm/SecondFundamentalFormDiscretization.h"
#include "../SecondFundamentalForm/MidedgeAngleSinFormulation.h"
#include "../SecondFundamentalForm/MidedgeAngleTanFormulation.h"
#include "../SecondFundamentalForm/MidedgeAverageFormulation.h"


class ElasticSetup
{
public:
	ElasticSetup()
	{
		restV.resize(0, 0);
		restF.resize(0, 0);
		clampedDOFs.clear();
		obs.clear();
		restEdgeDOFs.resize(0);

		thickness = 0;
		YoungsModulus = 0;
		PoissonsRatio = 0;
		density = 0;
		penaltyK = 0;
		pressure = 0;

		innerEta = 0;
		restFlat = true;

		maxStepSize = 1;
		perturb = 0;
		gravity.setZero();
//		framefreq = 1;
		numInterp = 1;

		abars.clear();
		bbars.clear();
		vertArea.clear();

        strecthingType = "";
		bendingType = "";
		sffType = "";
		restMeshPath = "";
		obstaclePath = "";

		initMeshPath = "";
		curMeshPath = "";
		curEdgeDOFsPath = "";

		clampedDOFsPath = "";
		pointForcesPath = "";

		outMeshPath = "";
		
	}

public:
	Eigen::MatrixXd restV;
	Eigen::MatrixXi restF; // mesh vertices of the original (unstitched) state

	std::vector<Obstacle> obs;

	Eigen::VectorXd restEdgeDOFs;
	Eigen::SparseMatrix<double> laplacian;
	std::map<int, double> clampedDOFs;
	std::map<int, double> pointForces;

	std::string sffType;
	std::shared_ptr<SecondFundamentalFormDiscretization> sff;
	
	double thickness;
	double YoungsModulus;
	double PoissonsRatio;
	double density;
	double penaltyK;
	double innerEta;

	double perturb;

	double pressure;
	bool restFlat;

	std::string bendingType;
    std::string strecthingType;
	
	double maxStepSize;

	Eigen::Vector3d gravity;
//	int framefreq;
	int numInterp; // number of interpolation steps before reaching clamped boundary

	// Derived from the above
	std::vector<Eigen::Matrix2d> abars;
	std::vector<Eigen::Matrix2d> bbars;

	//vert area
	std::vector<double> vertArea;

	std::string restMeshPath, obstaclePath, initMeshPath, curMeshPath, curEdgeDOFsPath, clampedDOFsPath, pointForcesPath, outMeshPath;

public:
	void buildRestFundamentalForms();
	void computeVertArea(const int nverts, const Eigen::MatrixXi& stitchedF);
	void computeLaplacian(const Eigen::MatrixXd restV, const Eigen::MatrixXi restF, const std::vector<Eigen::Vector3i>& bnd_edges, const Eigen::VectorXi& newIndex, const double nverts);
};
