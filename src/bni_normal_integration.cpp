#include <Eigen/Core>
#include <Eigen/Dense>
#include <Eigen/SparseCore>
#include <Eigen/IterativeLinearSolvers>

#include <QFile>
#include <QTextStream>
#include <QImage>
#include "bni_normal_integration.h"
#include <iostream>

/* TODO: try this lib:
https://amgcl.readthedocs.io/en/latest/tutorial/poisson3Db.html
*/

typedef Eigen::Triplet<double> Triple;

using namespace std;

double sigmoid(const double x, const double k = 1.0) {
	return 1.0 / (1.0 + exp(-x*k));
}

bool saveDepthMap(const QString &filename, int w, int h, std::vector<float> &z) {
	return false;
}

bool saveNormalMap(const QString &filename, int w, int h, std::vector<float> &normals) {
	QImage img(w, h, QImage::Format_ARGB32);
	for(int y = 0; y < h; y++) {
		uint8_t *line = img.scanLine(y);
		for(int x = 0; x < w; x++) {
			float *n = &normals[3*(x + y*w)];
			line[4*x + 0] = 255;
			line[4*x + 1] = floor(n[0]*255.0f);
			line[4*x + 2] = floor(n[1]*255.0f);
			line[4*x + 3] = floor(n[2]*255.0f);
		}
	}
	img.save(filename);
	return true;
}

bool savePly(const QString &filename, int w, int h, std::vector<float> &z) {
	QFile file(filename);
	bool success = file.open(QFile::WriteOnly);
	if(!success)
		return false;
	{
		QTextStream stream(&file);

		stream << "ply\n";
		stream << "format binary_little_endian 1.0\n";
		stream << "element vertex " << w*h << "\n";
		stream << "property float x\n";
		stream << "property float y\n";
		stream << "property float z\n";
		stream << "element face " << 2*(w-1)*(h-1) << "\n";
		stream << "property list uchar int vertex_index\n";
		stream << "end_header\n";
	}

	std::vector<float> vertices(w*h*3);
	for(int y = 0; y < h; y++) {
		for(int x = 0; x < w; x++) {
			int pos = x + y*w;
			float *start = &vertices[3*pos];
			start[0] = x;
			start[1] = y;
			start[2] = z[pos];
		}
	}
	std::vector<uint8_t> indices(13*2*(w-1)*(h-1));
	for(int y = 0; y < h-1; y++) {
		for(int x = 0; x < w-1; x++) {
			int pos = x + y*w;
			uint8_t *start = &indices[26*(x + y*(w-1))];
			start[0] = 3;
			int *face = (int *)(start + 1);
			assert(pos+w+1 < w*h);
			face[0] = pos;
			face[1] = pos+w;
			face[2] = pos+w+1;

			start += 13;
			start[0] = 3;
			face = (int *)(start + 1);
			face[0] = pos;
			face[1] = pos+w+1;
			face[2] = pos+1;
		}
	}
	file.write((const char *)vertices.data(), vertices.size()*4);
	file.write((const char *)indices.data(), indices.size());
	file.close();
	return true;
}

void bilinear_interpolation(float *data, uint32_t input_width,
							uint32_t input_height, uint32_t output_width,
							uint32_t output_height, float *output) {
	float x_ratio, y_ratio;

	if (output_width > 1) {
		x_ratio = ((float)input_width - 1.0) / ((float)output_width - 1.0);
	} else {
		x_ratio = 0;
	}

	if (output_height > 1) {
		y_ratio = ((float)input_height - 1.0) / ((float)output_height - 1.0);
	} else {
		y_ratio = 0;
	}

	for (int i = 0; i < output_height; i++) {
		for (int j = 0; j < output_width; j++) {
			float x_l = floor(x_ratio * (float)j);
			float y_l = floor(y_ratio * (float)i);
			float x_h = ceil(x_ratio * (float)j);
			float y_h = ceil(y_ratio * (float)i);

			float x_weight = (x_ratio * (float)j) - x_l;
			float y_weight = (y_ratio * (float)i) - y_l;

			float a = data[(int)y_l * input_width + (int)x_l];
			float b = data[(int)y_l * input_width + (int)x_h];
			float c = data[(int)y_h * input_width + (int)x_l];
			float d = data[(int)y_h * input_width + (int)x_h];

			float pixel = a * (1.0 - x_weight) * (1.0 - y_weight) +
					b * x_weight * (1.0 - y_weight) +
					c * y_weight * (1.0 - x_weight) +
					d * x_weight * y_weight;

			output[i * output_width + j] = pixel;
		}
	}
}


class NormalMap {
public:
	vector<float> normals;
	vector<float> heights;
	int w;
	int h;

	NormalMap up() { //halve resolution
		NormalMap scaled;
		scaled.w = w/2;
		scaled.h = h/2;

		scaled.normals.resize(scaled.w*scaled.h*3);
		for(int y = 0; y < scaled.h; y++) {
			for(int x = 0; x < scaled.w; x++) {
				float *p = &scaled.normals[3*(x + y*scaled.w)];
				for(int k = 0; k < 3; k++) {
					p[k] = (normals[k + 3*(2*x + 2*y*h)] +
							normals[k + 3*(2*x + 1 + 2*y*h)] +
							normals[k + 3*(2*x + (2*y + 1)*h)] +
							normals[k + 3*(2*x + 1 + (2*y +1)*h)])/4.0f;
				}
				//normalize:
				float length = sqrt(p[0]*p[0] + p[1]*p[1] + p[2]*p[2]);
				for(int k = 0; k < 3; k++)
					p[k] /= length;

			}
		}
		return scaled;
	}

	void pull(NormalMap &small) { //update normals
		heights.resize(w*h, 0);
		bilinear_interpolation(small.heights.data(), small.w, small.h, w, h, heights.data());
		/*for(int y = 0; y < h; y++) {
			for(int x = 0; x < w; x++) {
				heights[x + y*h] = small.heights[tx + ty*small.h];
			}
		}*/
	}
};

std::vector<float> bni_pyramid(std::function<bool(std::string s, int n)> progressed, int &w, int &h, std::vector<float> &normalmap,
							   double k,
							   double tolerance,
							   double solver_tolerance,
							   int max_iterations,
							   int max_solver_iterations,
							   int scale) {
	vector<NormalMap> pyramid;
	vector<int> widths;
	vector<int> heights;

	NormalMap m;
	m.w = w;
	m.h = h;
	m.normals = normalmap;
	pyramid.push_back(m);

	int min_size = 32;
	while(pyramid.back().w > min_size && pyramid.back().h > min_size) {
		pyramid.push_back(pyramid.back().up());
	}
	NormalMap &top = pyramid.back();
	top.heights.resize(top.w*top.h, 0.0f);

	cout << "Scale: " << scale << endl;
	for(int i = pyramid.size()-1; i >= scale; i--) {
		cout << "Level: " << i << endl;
		NormalMap &p = pyramid[i];
		saveNormalMap("testN_" + QString::number(i) + ".png", p.w, p.h, p.normals);
		if(i + 1 < pyramid.size())
			p.pull(pyramid[i+1]);
		bni_integrate(progressed, p.w, p.h, p.normals, p.heights, k, tolerance, solver_tolerance, max_iterations, max_solver_iterations);
		savePly("test_" + QString::number(i) + ".ply", p.w, p.h, p.heights);

	}
	NormalMap &result = pyramid[scale];
	w = result.w;
	h = result.h;
	return result.heights;
}

void bni_integrate(std::function<bool(std::string s, int n)> progressed, int w, int h, std::vector<float> &normalmap, std::vector<float> &heights,
				   double k,
				   double tolerance,
				   double solver_tolerance,
				   int max_iterations,
				   int max_solver_iterations) {

	cout << "Eigen using nthreads: " << Eigen::nbThreads( ) << endl;

	int n = w*h;
	Eigen::VectorXd nx(n);
	Eigen::VectorXd ny(n);
	Eigen::MatrixXd nz(h, w);

	for(int y = 0; y < h; y++) {
		for(int x= 0; x < w; x++) {
			int pos = x + y*w;
			nx(pos) = normalmap[pos*3+1];
			ny(pos) = normalmap[pos*3+0];
			nz(y, x) = -normalmap[pos*3+2];
		}
	}
	//BUILD A maps which encodes half derivatives (positive, negative, dx and dy)

	vector<Triple> triples;
	int offset =0;

	for(int y = 1; y < h; y++) {
		for(int x = 0; x < w; x++) {
			int pos = x + y*w;
			triples.push_back(Triple(offset + pos, pos, -nz(y, x)));
			triples.push_back(Triple(offset + pos, pos-w, nz(y, x)));
		}
	}
	offset += n;

	for(int y = 0; y < h-1; y++) {
		for(int x = 0; x < w; x++) {
			int pos = x + y*w;
			triples.push_back(Triple(offset + pos, pos, nz(y, x)));
			triples.push_back(Triple(offset + pos, pos+w, -nz(y, x)));

		}
	}
	offset += n;

	for(int y = 0; y < h; y++) {
		for(int x = 0; x < w-1; x++) {
			int pos = x + y*w;
			triples.push_back(Triple(offset + pos, pos, -nz(y, x)));
			triples.push_back(Triple(offset + pos, pos+1, nz(y, x)));
		}
	}
	offset += n;

	for(int y = 0; y < h; y++) {
		for(int x = 1; x < w; x++) {
			int pos = x + y*w;
			triples.push_back(Triple(offset + pos, pos, nz(y, x)));
			triples.push_back(Triple(offset + pos, pos-1, -nz(y, x)));

		}
	}
	offset += n;

	Eigen::SparseMatrix<double> A(n*4, n);
	A.setFromTriplets(triples.begin(), triples.end());



	Eigen::VectorXd b(n*4);
	b << -nx, -nx, -ny, -ny;

	Eigen::SparseMatrix<double> W(n*4, n*4);
	triples.clear();
	for(int i = 0; i < n*4; i++)
		triples.push_back(Triple(i, i, 0.5));
	W.setFromTriplets(triples.begin(), triples.end());

	Eigen::VectorXd z = Eigen::VectorXd::Zero(n);
	for(int i = 0; i < n; i++)
		z(i) = heights[i];

	Eigen::MatrixXd tmp = A*z - b;
	double energy = (tmp.transpose() * W * tmp)(0);
	double start_energy = energy;
	if(isnan(energy)) {
		throw "Accidentaccio!";
	}

	cout << "Energy : " << energy << endl;

	for(int i = 0; i < max_iterations; i++) {

		Eigen::SparseMatrix<double> A_mat = A.transpose()*W*A;
		Eigen::VectorXd b_vec = A.transpose()*W*b;

		Eigen::ConjugateGradient<Eigen::SparseMatrix<double>> solver;

		//Eigen::LeastSquaresConjugateGradient<Eigen::SparseMatrix<double>> solver;
		//Eigen::BiCGSTAB<Eigen::SparseMatrix<double> > solver;
		solver.compute(A_mat);
		solver.setTolerance(solver_tolerance);
		solver.setMaxIterations(max_solver_iterations);

		z = solver.solveWithGuess(b_vec, z);
		if (solver.info() != Eigen::Success) {
			double finalResidual = solver.error();
			cerr << "Max iter reached with error: " << finalResidual << endl;
			//			return std::vector<double>();
		}

		// Get the number of iterations
		int numIterations = solver.iterations();
		std::cout << "Number of iterations: " << numIterations << " error: " << solver.error() << " tolerance: " << solver_tolerance << std::endl;


		if(k == 0)
			break;

		Eigen::VectorXd wu = ((A.block(n, 0, n, n)*z).array().pow(2) -
							  (A.block(0, 0, n, n)*z).array().pow(2)).unaryExpr([k](double x) { return sigmoid(x, k); });
		Eigen::VectorXd wv = ((A.block(n*3, 0, n, n)*z).array().pow(2) -
							  (A.block(n*2, 0, n, n)*z).array().pow(2)).unaryExpr([k](double x) { return sigmoid(x, k); });

		triples.clear();
		for(int i = 0; i < n; i++) {
			triples.push_back(Triple(i, i, wu(i)));
			triples.push_back(Triple(i+n, i+n, 1.0 - wu(i)));
			triples.push_back(Triple(i+n*2, i+n*2, wv(i)));
			triples.push_back(Triple(i+n*3, i+n*3, 1 - wv(i)));
		}
		W.setFromTriplets(triples.begin(), triples.end());

		double energy_old = energy;
		tmp = A*z - b;
		energy = (tmp.transpose() * W * tmp)(0);
		cout << "Energy: " << energy << endl;

		double relative_energy = fabs(energy - energy_old) / energy_old;
		double total_progress = fabs(energy - start_energy) / start_energy;
		if(progressed)
			progressed("Integrating normals...", 100*(log(relative_energy) - log(tolerance))/(log(total_progress) - log(tolerance)));
		if(relative_energy < tolerance)
			break;
	}
	for(int i = 0; i < w*h; i++)
		heights[i] = z(i);
	//memcpy(&depthmap[0], z.data(), n*8);
}
