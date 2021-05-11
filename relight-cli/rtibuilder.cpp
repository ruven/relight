#include <assert.h>

#include <algorithm>

#include "rtibuilder.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QImage>
#include <QRunnable>
#include <QThreadPool>
#include <QtConcurrent>
#include <QFuture>


#include "../src/jpeg_decoder.h"
#include "../src/jpeg_encoder.h"

//#include "../src/pca.h"
#include "../src/eigenpca.h"

#include <Eigen/Core>

#include <set>
#include <iostream>

#include <math.h>
using namespace std;

/* reampling model:
 *
 * the parametrized octa space is [-1,-1][-1,-1] mapped from 0,0 to res,res
 *
 * here we need only the fromOcta. where we want 0,0 to be mapped in (-1, 0) and 7,7 to (0, 1)
 */

static void toOcta(Vector3f d, float &x, float &y, int resolution) {
	//convert to octa space [-1,1] diamond
	float s = fabs(d[0]) + fabs(d[1]) + fabs(d[2]);
	//rotated 45 degs this is scaled to [-1,1] square
	x = (d[0] + d[1])/s;
	y = (d[1] - d[0])/s;
	//bring it to [0,1] square]
	x = (x + 1.0f)/2.0f;
	y = (y + 1.0f)/2.0f;
	//scale from [0-7]
	x = std::max(0.0f, std::min(resolution-1.0f, x*(resolution-1.0f)));
	y = std::max(0.0f, std::min(resolution-1.0f, y*(resolution-1.0f)));
	assert(x >= 0 && x <= (resolution-1));
	assert(y >= 0 && y <= (resolution-1));
}

static Vector3f fromOcta(int x, int y, int resolution) {
	//rescale to [-1,1]
	float oX = 2.0f*x/float(resolution-1) -1.0f; //(((x + 0.5f)/(resolution-1)) - 0.5f);
	float oY = 2.0f*y/float(resolution-1) -1.0f; //(((y + 0.5f)/(resolution-1)) - 0.5f);
	
	//rotate 45 deg and keep it to [-1,1] diamond
	float X = (oX - oY)/2.0f;
	float Y = (oX + oY)/2.0f;
	
	Vector3f n(X, Y, 1.0f - fabs(X) -fabs(Y));
	if(n[2] < 0) //approx error.
		n[2] = 0;
	n = n/float(n.norm());
	return n;
}
RtiBuilder::RtiBuilder() {}
RtiBuilder::~RtiBuilder() {}

bool RtiBuilder::initFromFolder(const string &folder, std::function<bool(std::string stage, int percent)> *_callback) {
	
	callback = _callback;
	try {
		if(!imageset.initFromFolder(folder.c_str(), true, skip_image)) {
			error = "Failed imageset init.";
			return false;
		}
	} catch(QString e) {
		error = e.toStdString();
		return false;
	}
	if(crop[2] != 0) //some width specified
		imageset.crop(crop[0], crop[1], crop[2], crop[3]);

	width = imageset.width;
	height = imageset.height;
	lights = imageset.lights;
	return init();
}


bool RtiBuilder::initFromProject(const std::string &filename, std::function<bool(std::string stage, int percent)> *_callback) {
	callback = _callback;
	try {
		imageset.initFromProject(filename.c_str());
		//overwrite project crop if specified in builder.
		if(crop[2] != 0) //some width specified
			imageset.crop(crop[0], crop[1], crop[2], crop[3]);

		width = imageset.width;
		height = imageset.height;
		lights = imageset.lights;
	} catch(QString e) {
		error = e.toStdString();
		return false;
	}
	return init(_callback);
}

/*
void RtiBuilder::saveLightPixel(Color3f *p, int side, const QString &file) {
	QImage img(side, side, QImage::Format_RGB32);
	img.fill(qRgb(0, 0, 0));
	for(size_t i = 0; i < lights.size(); i++) {
		float dx, dy;
		toOcta(lights[i], dx, dy, side);
		int x = dx;
		int y = dy;
		
		img.setPixel(x, y, qRgb((int)p[i].r, (int)p[i].g, (int)p[i].b));
	}
	img.save(file);
}

void RtiBuilder::savePixel(Color3f *p, int side, const QString &file) {
	QImage img(resolution, resolution, QImage::Format_RGB32);
	img.fill(qRgb(0, 0, 0));
	for(uint32_t y = 0, i = 0; y < resolution; y++)
		for(uint32_t x = 0; x < resolution; x++, i++)
			img.setPixel(x, y, qRgb((int)p[i].r, (int)p[i].g, (int)p[i].b));
	img = img.scaled(side, side, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
	img.save(file);
}
*/

bool RtiBuilder::init(std::function<bool(std::string stage, int percent)> *_callback) {
	if((type == PTM || type == HSH || type == SH || type == H) && colorspace == MRGB) {
		error = "PTM and HSH do not support MRGB";
		return false;
	}

	if((type == RBF || type == BILINEAR) &&  (colorspace != MRGB && colorspace != MYCC)) {
		error = "RBF and BILINEAR support only MRGB and MYCC";
		return false;
	}

	callback = _callback;


	if(type == BILINEAR) {
		ndimensions = resolution*resolution;
		buildResampleMaps();
	} else {
		ndimensions = lights.size();
	}
	if(samplingram == 0) {
		cerr << "Sampling RAM must be > 0\n";
		return false;
	}

	imageset.setCallback(callback);

	try {
		//we don't actually need to store the samples, we can just add to (resample) add to PCA, or use to compute material.
		//collect a set of samples resampled
		PixelArray resample;
		imageset.sample(resample, ndimensions, [&](Pixel &sample, Pixel &resample) { this->resamplePixel(sample, resample); }, samplingram);
		nsamples = resample.npixels();

		pickBases(resample);
	} catch(std::exception e) {
		error = "Could not create a base.";
		return false;
	} catch(...) {
		error = "Could not create a base.";
		return false;
	}
	return true;
}


//assumes pixel intensities are already fixed.
MaterialBuilder RtiBuilder::pickBasePCA(PixelArray &sample) {
	
	
	//let's work pixel by pixel
	
	//let's weight worse errors:
	//1 compute weights
	//2 center should be in sum w_iX_i (so save it)
	//3 multiply each X_i by sqrt(w_i)
	//4 solve
	//5 now mean is in sqrt(w_i)X_i, use the other one.
	//6 compute the error per pixel, and update weihgts
	//loop

	MaterialBuilder mat;
	
	if(callback)
		if(!(*callback)("Computing PCA:", 0))
			throw std::string("Cancelled.");

	if(colorspace == MRGB) {
		uint32_t dim = sample.components()*3;
		
		vector<double> record(dim);
		
		vector<double> means;
		PCA pca(dim, nsamples);
		means.resize(dim, 0.0);

		//compute mean
		for(Pixel &pixel: sample) {
			for(uint32_t k = 0; k < sample.components(); k ++) {
				Color3f &c = pixel[k];
				means[k*3 + 0] += double(c.r);
				means[k*3 + 1] += double(c.g);
				means[k*3 + 2] += double(c.b);
			}
		}
		for(double &d: means)
			d /= nsamples;

		if(callback && !(*callback)("Computing PCA:", 5))
			throw std::string("Cancelled.");

		for(uint32_t i = 0; i < sample.size(); i++) {
			Pixel &pixel = sample[i];
			for(uint32_t k = 0; k < sample.components(); k ++) {
				Color3f c = pixel[k];
				record[k*3 + 0] = (double(c.r) - means[k*3+0]);
				record[k*3 + 1] = (double(c.g) - means[k*3+1]);
				record[k*3 + 2] = (double(c.b) - means[k*3+2]);
			}
			pca.setRecord(i, record);
		}

		if(callback && !(*callback)("Computing PCA:", 10))
			throw std::string("Cancelled.");

		pca.solve(nplanes);

		mat.mean.resize(dim, 0.0f);

		//ensure the mean is within range (might be slightly negative due to resampling bilinear
		for(uint32_t k = 0; k < dim; k++)
			mat.mean[k] = std::max(0.0f, std::min(255.0f, float(means[k])));

		mat.proj.resize(nplanes*dim);

		for(uint32_t p = 0; p < nplanes; p++) {
			for(uint32_t k = 0; k < dim; k++)
				mat.proj[k + p*dim] = float(pca.proj()(k, p));
		}

	} else { //MYCC!
		uint32_t dim = sample.components();
		
		vector<double> record(dim);
		vector<double> means;
		
		for(int component = 0; component < 3; component++) {
			std::vector<PCA *> pcas;
			means.resize(dim, 0.0);
			
			PCA pca(dim, nsamples);
			
			//compute mean
			for(Pixel &pixel: sample) {
				for(uint32_t k = 0; k < sample.components(); k ++) {
					Color3f &c = pixel[k];
					means[k] += double(c[component]);
				}
			}
			for(double &d: means)
				d /= nsamples;
			
			
			for(uint32_t i = 0; i < nsamples; i++) {
				//TODO iterate over rawdata.
				Pixel &pixel = sample[i];
				for(uint32_t k = 0; k < sample.components(); k ++) {
					Color3f &c = pixel[k];
					record[k] = (c[component] - means[k]);
				}
				pca.setRecord(i, record);
			}

			
			pca.solve(yccplanes[component]);

			mat.mean.resize(dim*3, 0.0f);
			//ensure the mean is within range (might be slightly negative due to resampling bilinear
			for(uint32_t k = 0; k < dim; k++)
				mat.mean[k*3 + component] = std::max(0.0, std::min(255.0, means[k])); // pca->mean()[k]));

			/*		for(int k = 0; k < dim; k += 3) {
				Color3f &c = *(Color3f *)&mat.mean[k];
				c = lab2rgb(c);
				} */
			mat.proj.resize(nplanes*dim*3, 0.0f); //actual;y redundant but harmless

			//ycc has interleaved first then the remaining y components
			for(uint32_t yp = 0; yp < yccplanes[component]; yp++) {
				uint32_t p = 0;
				if(component == 0) {
					if(yp < yccplanes[1])
						p = yp*3 + component;
					else if(component == 0)
						p = yccplanes[1]*2 + yp;
				} else
					p = yp*3 + component;
				for(uint32_t k = 0; k < dim; k++)
					mat.proj[k*3 + component + p*dim*3] = pca.proj()(k, yp);
			}


			if(callback && !(*callback)("Computing PCA:", 100*component/3))
				throw std::string("Cancelled.");
		}
	}
	//normalize coeffs
	uint32_t dim = sample.components()*3;
	float *c = mat.proj.data(); //colptr(0);

	for(uint32_t p = 0; p < nplanes; p++) {
		double e = 0.0;
		for(uint32_t k = 0; k < dim; k++)
			e += c[k + p*dim]*c[k+p*dim];
		e = sqrt(e);
		for(uint32_t k = 0; k < dim; k++)
			c[k+p*dim] /= e;
	}

	if(callback && !(*callback)("Computing PCA:", 100))
		throw std::string("Cancelled.");
	return mat;
}

//assume directional lights and intensity already fixed.
MaterialBuilder RtiBuilder::pickBasePTM(std::vector<Vector3f> &lights) {
	
	/* every light is linear combination of 6 pol coeff
		b = w00x00 + w01x01 + ... w05x
	solution is closed form matrix
	b = Ax  matrix A is 6 col Time 116 rows.
	//we only want the least square sol.
	invert it:
	x = (At * A)^-1*At*b
	*/

	uint32_t dim = ndimensions*3;
	MaterialBuilder mat;
	mat.mean.resize(dim, 0.0);
	
	Eigen::MatrixXd A(lights.size(), 6);
	for(uint32_t l = 0; l < lights.size(); l++) {
		Vector3f &light = lights[l];
		A(l, 0) = 1.0;
		A(l, 1) = double(light[0]);
		A(l, 2) = double(light[1]);
		A(l, 3) = double(light[0]*light[0]);
		A(l, 4) = double(light[0]*light[1]);
		A(l, 5) = double(light[1]*light[1]);
	}

	Eigen::MatrixXd iA = (A.transpose()*A).inverse()*A.transpose();
	
	if(colorspace == LRGB) {
		assert(nplanes == 9);
		//we could generalize for different polinomials
		
		//nplanes should be 9 here!
		std::vector<float> &proj = mat.proj;
		proj.resize(nplanes*dim, 0.0);
		for(uint32_t p = 0; p < nplanes; p++) {
			for(uint32_t k = 0; k < lights.size(); k++) {
				uint32_t off = k*3 + p*dim;
				if(p >= 3) {
					proj[off+0] = float(0.2125*iA(p-3, k));
					proj[off+1] = float(0.7154*iA(p-3, k));
					proj[off+2] = float(0.0721*iA(p-3, k));
				} else {
					proj[off + p] = 1.0f/lights.size();
				}
			}
		}
	} else {
		assert(colorspace == RGB && nplanes == 18);
		
		//nplanes should be 18 here!
		std::vector<float> &proj = mat.proj;
		proj.resize(nplanes*dim, 0.0);
		for(uint32_t p = 0; p < nplanes; p += 3) {
			for(uint32_t k = 0; k < lights.size(); k ++) {
				proj[k*3+0 + (p+0)*dim] = proj[k*3+1 + (p+1)*dim] = proj[k*3+2 + (p+2)*dim] = float(iA(p/3, k));
			}
		}
	}
	return mat;
}

//assume directional lights and intensity already fixed.

MaterialBuilder RtiBuilder::pickBaseHSH(std::vector<Vector3f> &lights, Rti::Type base) {
	uint32_t dim = ndimensions*3;
	MaterialBuilder mat;
	mat.mean.resize(dim, 0.0);
	
	vector<float> lweights;

	Eigen::MatrixXd A(lights.size(), nplanes/3);
	for(uint32_t l = 0; l < lights.size(); l++) {
		Vector3f &light = lights[l];
		switch(base) {
		case HSH:
			lweights = lightWeightsHsh(light[0], light[1]);
			break;
		case SH:
			lweights = lightWeightsSh(light[0], light[1]);
			break;
		case H:
			lweights = lightWeightsH(light[0], light[1]);
			break;
		default:
			break;
		}
		for(uint32_t p = 0; p < nplanes/3; p++)
			A(l, p) = double(lweights[p]);
	}
	Eigen::MatrixXd iA = (A.transpose()*A).inverse()*A.transpose();

	assert(nplanes == 27 || nplanes == 12);
	
	std::vector<float> &proj = mat.proj;
	proj.resize(nplanes*dim, 0.0);
	for(uint32_t p = 0; p < nplanes; p += 3) {
		for(uint32_t k = 0; k < lights.size(); k ++) {
			proj[k*3+0 + (p+0)*dim] = proj[k*3+1 + (p+1)*dim] = proj[k*3+2 + (p+2)*dim] = iA(p/3, k);
		}
	}
	return mat;
}

void RtiBuilder::pickBases(PixelArray &sample) {
	//rbf can't 3d, bilinear just resample (and then single base)
	if(!imageset.light3d) {
		materialbuilder = pickBase(sample, imageset.lights);

	} else {

		if(type == RBF || type == BILINEAR) {

			materialbuilder = pickBase(sample, imageset.lights); //lights are unused for rbf

		} else {
			resample_height = 8;
			resample_width = 8;
			materialbuilders.resize(resample_width * resample_height);

			for(int y = 0; y < resample_height; y++) {
				for(int x = 0; x < resample_width; x++) {
					int pixel_x = imageset.width*x/(resample_width-1);
					int pixel_y = imageset.height*y/(resample_height-1);
					auto relights = relativeLights(pixel_x, pixel_y);
					materialbuilders[x + y*resample_width] = pickBase(sample, relights);
				}
			}
		}
	}
	minmaxMaterial(sample);
	finalizeMaterial();
}


MaterialBuilder RtiBuilder::pickBase(PixelArray &sample, std::vector<Vector3f> &lights) {


	vector<Vector3f> directions = lights;
	for(Vector3f &light: directions)
		light.normalize();


	//TODO PTM and HSH need only 1 pass to compute max and min, do not need to read tge samples in an array, can be done on the fly.
	//RBF and BILINEAR instead need 2 passes: 1 for the PCA and the second for the min/max.
	MaterialBuilder builder;
	switch(type) {
	case RBF:
	case BILINEAR: builder = pickBasePCA(sample); break;
	case PTM:      builder = pickBasePTM(directions); break;
	case HSH:
	case SH:
	case H:        builder = pickBaseHSH(directions, type); break;
	default: cerr << "Unknown basis" << endl; exit(0);
	}
/*
	uint32_t dim = sample.components()*3;
	for(uint32_t p = 0; p < nplanes; p += 3) {
		for(uint32_t k = 0; k < lights.size(); k ++) {
			proj[k*3+0 + (p+0)*dim] = proj[k*3+1 + (p+1)*dim] = proj[k*3+2 + (p+2)*dim] = iA(p/3, k);
		}
	} */

	return builder;
}

void RtiBuilder::minmaxMaterial(PixelArray &sample) {
	uint32_t dim = sample.components()*3;

	//material.planes.clear();
	material.planes.resize(nplanes);

	//normalize
	if(type == RBF || type == BILINEAR) {
		float *c = materialbuilder.proj.data(); //colptr(0);
		for(uint32_t p = 0; p < nplanes; p++) {
			Material::Plane &plane = material.planes[p];
			for(uint32_t k = 0; k < dim; k++)
				plane.range = std::max(plane.range, fabs(c[k + p*dim]));

			//basis coefficients can be negative, usually centered in zero
			//so quantization formula is 127 + range*eigen,  hence scaling range (which is based on fabs)
			plane.range = 127/plane.range;
		}
	}


	//TODO: range is need only for MRGB and MYCC


	if(callback && !(*callback)("Coefficients quantization:", 0))
		throw std::string("Cancelled.");

	//TODO workers to speed up this.
	for(uint32_t i = 0; i < sample.npixels(); i++) {
		if(callback && (i % 8000) == 0)
			if(!(*callback)("Coefficients quantization:", 100*i/sample.npixels()))
				throw std::string("Cancelled.");
		//TODO should interpolate instead!
		//TODO for rbf and bilinear we don't interpolate the grid (rbf can't bilinear resemple the lights)
		vector<float> principal = toPrincipal(sample[i]);

		//find max and min of coefficients
		for(uint32_t p = 0; p < nplanes; p++) {
			Material::Plane &plane = material.planes[p];
			plane.min = std::min(principal[p], plane.min);
			plane.max = std::max(principal[p], plane.max);
		}
	}
}

void RtiBuilder::finalizeMaterial() {
	float maxscale = 0.0f;
	//ensure scale is the same for all materials
	for(auto &plane: material.planes)
		maxscale = std::max(plane.max - plane.min, maxscale);

	//tested different scales for different coefficients (taking into account quantization for instance)
	// for all datasets the quality/space is worse.
	//rangecompress allows better quality at a large cost.
	for(Material::Plane &plane: material.planes) {
		plane.scale = rangecompress*(plane.max - plane.min) + (1 - rangecompress)*maxscale;
		plane.bias = -plane.min/plane.scale;
		plane.scale /= 255.0f;
	}

	if(callback && !(*callback)("Coefficients quantization:", 100))
		throw std::string("Cancelled.");
	//	estimateError(sample, indices, weights);
}


void RtiBuilder::estimateError(PixelArray &sample, std::vector<float> &weights) {
	weights.clear();
	weights.resize(sample.npixels(), 0.0f);
	uint32_t dim = sample.components()*3;
	double e = 0.0;
	double m = 0.0;
	for(size_t i = 0; i < sample.npixels(); i++) {
		MaterialBuilder &matb = materialbuilder;
		Material &mat = material;
		
		vector<float> principal = toPrincipal(sample[i], materialbuilder);
		for(size_t p = 0; p < principal.size(); p++) {
			Material::Plane &plane = mat.planes[p];
			principal[p] = plane.quantize(principal[p]);
			principal[p] = plane.dequantize(principal[p]);
		}
		
		vector<float> variable(dim, 0.0f);
		for(uint32_t k = 0; k < dim; k++)
			variable[k] = matb.mean[k];
		
		for(uint32_t p = 0; p < nplanes; p++) {
			float *eigen = matb.proj.data() + p*dim;//colptr(k);
			for(uint32_t k = 0; k < dim; k++)
				variable[k] += principal[p]*eigen[k];
			
			/*for(int y = 0; y < resolution; y++) {
				for(int x = 0; x < resolution; x++) {
					int o = (x + y*resolution)*3;
					variable[o + 0] += principal[p]*eigen[o+0];
					variable[o + 1] += principal[p]*eigen[o+1];
					variable[o + 2] += principal[p]*eigen[o+2];
				}
			}*/
		}
		float *s = (float *)sample[i].data();
		double se = 0.0;
		for(uint32_t k = 0; k < variable.size(); k++) {
			double d = variable[k] - s[k];
			se += d*d;
		}
		for(uint32_t k = 0; k < variable.size()/3; k++) {
			//			float O = variable[k*3+0] + variable[k*3+1] + variable[k*3+2];
			//			float S = s[k*3+0] + s[k*3+1] + s[k*3+2];
			
			float Or = (variable[k*3+1] - variable[k*3+0]);
			float Ob = (variable[k*3+1] - variable[k*3+2]);
			float Sr = (s[k*3+1] - s[k*3+0]);
			float Sb = (s[k*3+1] - s[k*3+2]);
			//weights[i] += pow(Sr, 2.0f);
			weights[i] += pow(Or - Sr, 4.0f) + pow(Ob - Sb, 4.0f);
		}
		e += se;
		se = sqrt(se);
		//weights[i] += se;
	}
	//normalization of weights
	
	e = sqrt(e/(sample.size()*3));
	
	for(float w: weights)
		m += w;
	
	for(float &w: weights)
		w = (w/m)*weights.size();
}

struct SwitchCost {
	int m0;
	int m1;
	double cost;
	SwitchCost(int _m0 = 0, int _m1 = 0, double _cost = 0):
		m0(_m0), m1(_m1), cost(_cost) {}
	//sort inverted from big to small
	bool operator<(const SwitchCost &c) { return fabs(cost) > fabs(c.cost); }
};


bool RtiBuilder::saveJSON(QDir &dir, int quality) {
	//save info.json
	QFile info(dir.filePath("info.json"));
	info.open(QFile::WriteOnly);
	QTextStream stream(&info);
	stream << "{\n\"width\": " << width << ", \"height\": " << height << ",\n"
		   << "\"format\": \"jpg\",\n";
	stream << "\"type\":\"";
	switch(type) {
	case PTM: stream << "ptm"; break;
	case HSH: stream << "hsh"; break;
	case SH:  stream << "sh"; break;
	case H:   stream << "h"; break;
	case RBF: stream << "rbf"; break;
	case BILINEAR: stream << "bilinear"; break;
	default: error = "Unknown RTI type"; return false;
	}
	stream << "\",\n";
	if(type == BILINEAR)
		stream << "\"resolution\": " << resolution << ",\n";
	
	stream << "\"colorspace\":\"";
	switch(colorspace) {
	case RGB: stream << "rgb"; break;
	case LRGB: stream << "lrgb"; break;
	case YCC: stream << "ycc"; break;
	case MRGB: stream << "mrgb"; break;
	case MYCC: stream << "mycc"; break;
	default: error = "Unknown RTI colorspace."; return false;
	}
	stream << "\",\n";
	
	if(lights.size()) {
		if(type == RBF)
			stream << "\"sigma\": " << sigma << ",\n";
		stream << "\"lights\": [";
		for(uint32_t i = 0; i < imageset.lights.size(); i++) {
			Vector3f &l = imageset.lights[i];
			stream << QString::number(l[0], 'f', 3) << ", " << QString::number(l[1], 'f', 3) << ", " << QString::number(l[2], 'f', 3);
			if(i != imageset.lights.size()-1)
				stream << ", ";
		}
		stream << "],\n";
	}
	if(colorspace == MYCC)
		stream << "\"yccplanes\": [" << yccplanes[0] << ", " << yccplanes[1] << ", " << yccplanes[2] << "],\n";
	else
		stream << "\"nplanes\": " << nplanes << ",\n";
	
	stream << "\"quality\": " << quality << ",\n";
	
	if(type == RBF || type == BILINEAR) {
		stream << "\"basis\": [\n";
		for(size_t i = 0; i < basis.size(); i++) {
			if(i != 0) {
				stream << ",";
				if((i % 80) == 0)
					stream << "\n";
			}
			stream << (int)basis[i];
		}
		stream << "],\n";
	}
	
	stream << "\"materials\": [\n";
	stream << "{\n";
	if(colorspace == MRGB || colorspace == MYCC) {
		stream<< " \"range\": [";
		for(uint32_t p = 0; p < nplanes; p++) {
			if(p != 0) stream << ",";
			stream << range[p];
		}
		stream << "],\n";
	}
	stream << " \"scale\": [";
	for(uint32_t p = 0; p < nplanes; p++) {
		if(p != 0) stream << ",";
		stream << scale[p];
	}
	stream << "],\n \"bias\": [";
	for(uint32_t p = 0; p < nplanes; p++) {
		if(p != 0) stream << ",";
		stream << bias[p];
	}

	stream << "] }";
	stream << "\n";

	
	stream << "]\n";
	stream << "}\n";
	info.close();
	return true;
}

Vector3f extractMean(Pixel &pixels, int n) {
	double m[3] = { 0.0, 0.0, 0.0 };
	for(int i = 0; i < n; i++) {
		Color3f &c = pixels[i];
		m[0] += c[0];
		m[1] += c[1];
		m[2] += c[2];
	}
	return Vector3f(m[0]/n, m[1]/n, m[2]/n);
}


Vector3f extractMedian(Pixel &pixels, int n) {

	Vector3f m;
	std::vector<float> a(n);
	for(int k = 0; k < 3; k++) {
		for(int i = 0; i < n; i++)
			a[i] = pixels[i][k];

		auto first = a.begin();
		auto last = a.end();
		auto middle = first + 7*(last - first)/8;
		std::nth_element(first, middle, last); // can specify comparator as optional 4th arg
		m[k] = *middle;
	}

	return m;
}


Vector3f RtiBuilder::getNormalThreeLights(vector<float> &pri) {
	static bool init = true;

	Eigen::Matrix3f T;
	static std::vector<float> w0, w1, w2;

	if(init) {
		init = false;

		float a = M_PI/4.0f;
		float b = M_PI/6.0f;
		Vector3f l0(sin(a)*cos(1*b), sin(a)*sin(1*b), cos(a));
		Vector3f l1(sin(a)*cos(5*b), sin(a)*sin(5*b), cos(a));
		Vector3f l2(sin(a)*cos(9*b), sin(a)*sin(9*b), cos(a));

		T << l0[0], l0[1], l0[2],
				l1[0], l1[1], l1[2],
				l2[0], l2[1], l2[2];

		T = T.inverse().eval();

		w0 = lightWeights(l0[0], l0[1]);
		w1 = lightWeights(l1[0], l1[1]);
		w2 = lightWeights(l2[0], l2[1]);
	}

	Eigen::Vector3f bright(0, 0, 0);

	if(colorspace == RGB) {
		for(uint32_t p = 0; p < nplanes; p += 3) {
			bright[0] += w0[p/3]*pri[p] + w0[p/3]*pri[p+1] + w0[p/3]*pri[p+2];
			bright[1] += w1[p/3]*pri[p] + w1[p/3]*pri[p+1] + w1[p/3]*pri[p+2];
			bright[2] += w2[p/3]*pri[p] + w2[p/3]*pri[p+1] + w2[p/3]*pri[p+2];
		}
	}
	else if (colorspace == MRGB) { //seems like weights are multiplied by 255 in rbf!
		bright[0] = w0[0] + w0[1] + w0[2];
		bright[1] = w1[0] + w1[1] + w1[2];
		bright[2] = w2[0] + w2[1] + w2[2];

		for (uint32_t p = 0; p < nplanes; p++) {
			Material::Plane &plane = material.planes[p];
			float val = pri[p];
			bright[0] += val*(w0[3 * (p + 1) + 0] + w0[3 * (p + 1) + 1] + w0[3 * (p + 1) + 2] - 3 * 127) / plane.range;
			bright[1] += val*(w1[3 * (p + 1) + 0] + w1[3 * (p + 1) + 1] + w1[3 * (p + 1) + 2] - 3 * 127) / plane.range;
			bright[2] += val*(w2[3 * (p + 1) + 0] + w2[3 * (p + 1) + 1] + w2[3 * (p + 1) + 2] - 3 * 127) / plane.range;
		}
	}

	Eigen::Vector3f N = T*bright;
	Vector3f n(N[0], N[1], N[2]);


	n = n/n.norm();
	n[0] = (n[0] + 1.0f)/2.0f;
	n[1] = (n[1] + 1.0f)/2.0f;
	n[2] = (n[2] + 1.0f)/2.0f;

	return n;
}


class Worker {
public:
	RtiBuilder &b;
	vector<vector<uint8_t>> line;
	vector<uchar> normals;
	vector<uchar> means;
	vector<uchar> medians;
	PixelArray sample;
	PixelArray resample;
	
	Worker(RtiBuilder &_builder): b(_builder) {
		uint32_t njpegs = (b.nplanes-1)/3 + 1;
		line.resize(njpegs);
		for(auto &p: line)
			p.resize(b.width*3, 0);
		normals.resize(b.width*3);
		means.resize(b.width*3);
		medians.resize(b.width*3);
		sample.resize(b.width, b.lights.size());
		resample.resize(b.width, b.ndimensions);

		//setAutodelete(false);
	}

	void run() {
		b.processLine(sample, resample, line, normals, means, medians);
	}
};

size_t RtiBuilder::save(const string &output, int quality) {
	uint32_t dim = ndimensions*3;
	
	QDir dir(output.c_str());
	if(!dir.exists()) {
		QDir here("./");
		if(!here.mkdir(output.c_str())) {
			error = "Could not create output dir.";
			return 0;
		}
	}
	
	//update scale bias and range in Rti structure
	scale.resize(nplanes);
	bias.resize(nplanes);
	range.resize(nplanes);
	
	for(uint32_t p = 0; p < nplanes; p++) {
		scale[p] = material.planes[p].scale;
		bias[p]  = material.planes[p].bias;
		if(colorspace == MRGB || colorspace == MYCC)
			range[p] = material.planes[p].range;
	}
	
	if(type == RBF || type == BILINEAR) {
		
		if(colorspace == MRGB || colorspace == MYCC) { //ycc should only store 1 component!

			for(uint32_t p = 0; p < ndimensions*3; p++)
				basis.push_back((int)(materialbuilder.mean[p]));



			for(uint32_t p = 0; p < nplanes; p++) {
				Material::Plane &plane = material.planes[p];
				float *eigen = materialbuilder.proj.data() + p*dim;
				for(uint32_t k = 0; k < ndimensions*3; k++) {
					basis.push_back((int)(127 + plane.range*eigen[k]));
				}
			}
		}
	}

	
	//TODO error control
	bool ok = saveJSON(dir, quality);
	if(!ok) return 0;
	
	//save materials as a single png
	
	if(type == RBF || type == BILINEAR) {
		if(type == BILINEAR) {
			
			QImage img(resolution*(nplanes+1), resolution, QImage::Format_RGB32);

			for(uint32_t y = 0; y < resolution; y++) {
				for(uint32_t x = 0; x < resolution; x++) {
					uint32_t o = (x + y*resolution)*3;
					int r = (int)materialbuilder.mean[o+0];
					int g = (int)materialbuilder.mean[o+1];
					int b = (int)materialbuilder.mean[o+2];
					img.setPixel(x, y, qRgb(r, g, b));
				}
			}
			for(uint32_t p = 0; p < nplanes; p++) {
				Material::Plane &plane = material.planes[p];
				float *eigen = materialbuilder.proj.data() + p*dim;
				uint32_t X = (p+1)*resolution;
				for(uint32_t y = 0; y < resolution; y++) {
					for(uint32_t x = 0; x < resolution; x++) {
						uint32_t o = (x + y*resolution)*3;
						int r = (int)(127 + plane.range*eigen[o+0]);
						int g = (int)(127 + plane.range*eigen[o+1]);
						int b = (int)(127 + plane.range*eigen[o+2]);
						img.setPixel(X + x, y, qRgb(r, g, b));
					}
				}
			}

			img.save(dir.filePath("materials.png"));
			
		} else if(type == RBF) {
			
			int side = 32;
			QImage img(side*(nplanes+1), side, QImage::Format_RGB32);
			img.fill(qRgb(0, 0, 0));
			for(uint32_t i = 0; i < imageset.lights.size(); i++) {
				float dx, dy;
				toOcta(imageset.lights[i], dx, dy, side);
				int x = dx;
				int y = dy;
				int r = (int)materialbuilder.mean[i*3+0];
				int g = (int)materialbuilder.mean[i*3+1];
				int b = (int)materialbuilder.mean[i*3+2];
				img.setPixel(0 + x, y, qRgb(r, g, b));
			}
			for(uint32_t p = 0; p < nplanes; p++) {
				Material::Plane &plane = material.planes[p];
				float *eigen = materialbuilder.proj.data() + p*dim;
				uint32_t X = (p+1)*side;
				for(size_t i = 0; i < imageset.lights.size(); i++) {
					float dx, dy;
					toOcta(imageset.lights[i], dx, dy, side);
					uint32_t x = dx;
					int y = dy;
					int r = (int)(127 + plane.range*eigen[i*3+0]);
					int g = (int)(127 + plane.range*eigen[i*3+1]);
					int b = (int)(127 + plane.range*eigen[i*3+2]);
					img.setPixel(X + x, y, qRgb(r, g, b));
				}
			}

			img.save(dir.filePath("materials.png"));
		}
	}
	
	//the image is processed one row at a time
	uint32_t njpegs = (nplanes-1)/3 + 1;
	vector<vector<uint8_t>> line(njpegs); //row in the new base.
	for(auto &p: line)
		p.resize(width*3, 0);
	
	vector<JpegEncoder *> encoders(njpegs);
	
	for(uint32_t i = 0; i < encoders.size(); i++) {
		encoders[i] = new JpegEncoder();
		encoders[i]->setQuality(quality);
		encoders[i]->setColorSpace(JCS_RGB, 3);
		encoders[i]->setJpegColorSpace(JCS_YCbCr);
		
		if(!chromasubsampling)
			encoders[i]->setChromaSubsampling(false);
		
		else {
			if(colorspace == MRGB)
				encoders[i]->setChromaSubsampling(false);
			
			else if(colorspace == YCC) {
				encoders[i]->setChromaSubsampling(i < yccplanes[0]);
				
			} else {
				encoders[i]->setChromaSubsampling(true);
			}
		}
		
		encoders[i]->init(dir.filePath("plane_%1.jpg").arg(i).toStdString().c_str(), width, height);
	}

	//second reading.
	imageset.restart();

	//TODO
	QImage normals(width, height, QImage::Format_RGB32);
	QImage means  (width, height, QImage::Format_RGB32);
	QImage medians(width, height, QImage::Format_RGB32);


	//colorspace check
	if (savenormals) {
		//init matrix for light computation (bleargh, static in function)
		vector<float> dummy(nplanes, 0.0f);
		getNormalThreeLights(dummy);
		if (colorspace != RGB && colorspace != MRGB) {
			cerr << "NO NORMALS (unsupported colorspace: RGB and MRGB only supported!)" << endl;
			savenormals = false;
		}
	}

	size_t nworkers = 8;
	vector<Worker *> workers(height, nullptr);
	for(size_t i = 0; i < nworkers; i++) {
		workers[i] = new Worker(*this);
	}
	vector<QFuture<void>> futures(height);

	QThreadPool pool;
	pool.setMaxThreadCount(nworkers);

	for(uint32_t y = 0; y < height + nworkers; y++) {
		if(callback && y > 0) {
			bool keep_going = (*callback)("Saving:", 100*(y)/(height + nworkers-1));
			if(!keep_going) {
				cout << "TODO: clean up directory, we are already saving!" << endl;
				break;
			}
		}
		if(y >= nworkers) {
			futures[y - nworkers].waitForFinished();

			Worker *doneworker = workers[y - nworkers];

			for(uint32_t x = 0; x < width; x++) {
				if (savenormals)
					normals.setPixel(x, y- nworkers, qRgb(doneworker->normals[x*3], 0, doneworker->normals[x*3+1]));
				//normals.setPixel(x, y- nworkers, qRgb(doneworker->normals[x*3], doneworker->normals[x*3+1], doneworker->normals[x*3+2]));
				if(savemeans)
					means.setPixel(x, y- nworkers, qRgb(doneworker->means[x*3], doneworker->means[x*3+1], doneworker->means[x*3+2]));
				if(savemedians)
					medians.setPixel(x, y- nworkers, qRgb(doneworker->medians[x*3], doneworker->medians[x*3+1], doneworker->medians[x*3+2]));
			}
			for(size_t j = 0; j < encoders.size(); j++)
				encoders[j]->writeRows(doneworker->line[j].data(), 1);
			if(y < height)
				workers[y] = doneworker;
			else
				delete doneworker;
		}

		if(y < height) {
			Worker *worker = workers[y];
			assert(worker != nullptr);
			imageset.readLine(worker->sample);

			futures[y] = QtConcurrent::run(&pool, [worker](){worker->run(); });
		}






		//classify samples
		//lower error but increase size of the files (why?)
		//getPixelBestMaterial(resample, indices);
		//getPixelMaterial(resample, indices);
		/*
		for(uint32_t x = 0; x < width; x++) {
			uint32_t m = indices[x];
			Material &mat = materials[m];
			
			segments.setPixel(x, y, m);
			
			vector<float> pri = toPrincipal(m, (float *)(resample(x)));

			if (savenormals) {
				Vector3f n = getNormalThreeLights(pri);
				normals.setPixel(x, y, qRgb(255 * n[0], 255 * n[1], 255 * n[2]));
			}

			if(savemeans) {
				Vector3f n = extractMean(sample(x), lights.size());
				means.setPixel(x, y, qRgb(n[0], n[1], n[2]));
			}

			if(savemedians) {
				Vector3f n = extractMedian(sample(x), lights.size());
				medians.setPixel(x, y, qRgb(n[0], n[1], n[2]));
			}

			if(colorspace == LRGB){

				for(uint32_t j = 0; j < nplanes/3; j++) {
					for(uint32_t c = 0; c < 3; c++) {
						uint32_t p = j*3 + c;
						if(j >= 1)
							pri[p] = mat.planes[p].quantize(pri[p]);
						line[j][x*3 + c] = pri[p];
					}
				}
				
			} else {

				for(uint32_t j = 0; j < nplanes/3; j++) {
					for(uint32_t c = 0; c < 3; c++) {
						uint32_t p = j*3 + c;
						line[j][x*3 + c] = mat.planes[p].quantize(pri[p]);
					}
				}
			}
		}*/
	}

	size_t total = 0;
	for(size_t p = 0; p < encoders.size(); p++) {
		size_t s = encoders[p]->finish();
		total += s;
	}

	if(savenormals)
		normals.save(dir.filePath("normals.png"));

	if(savemeans)
		means.save(dir.filePath("means.png"));

	if(savemedians)
		medians.save(dir.filePath("medians.png"));

	QFileInfo infoinfo(dir.filePath("info.json"));
	total += infoinfo.size();
	
	return total;
}

void RtiBuilder::processLine(PixelArray &sample, PixelArray &resample, std::vector<std::vector<uint8_t>> &line,
							 std::vector<uchar> &normals, std::vector<uchar> &means, std::vector<uchar> &medians) {

	for(uint32_t x = 0; x < width; x++)
		resamplePixel(sample[x], resample[x]);


	for(uint32_t x = 0; x < width; x++) {
		vector<float> pri = toPrincipal(resample[x]);

		if (savenormals) {
			Vector3f n = getNormalThreeLights(pri);
			normals[x*3+0] = 255*n[0];
			normals[x*3+1] = 255*n[1];
			normals[x*3+2] = 255*n[2];
		}

		if(savemeans) {
			Vector3f n = extractMean(sample[x], lights.size());
			means[x*3+0] = n[0];
			means[x*3+1] = n[1];
			means[x*3+2] = n[2];
		}

		if(savemedians) {
			Vector3f n = extractMedian(sample[x], lights.size());
			medians[x*3+0] = n[0];
			medians[x*3+1] = n[1];
			medians[x*3+2] = n[2];
		}

		if(colorspace == LRGB){

			for(uint32_t j = 0; j < nplanes/3; j++) {
				for(uint32_t c = 0; c < 3; c++) {
					uint32_t p = j*3 + c;
					if(j >= 1)
						pri[p] = material.planes[p].quantize(pri[p]);
					line[j][x*3 + c] = pri[p];
				}
			}

		} else {

			for(uint32_t j = 0; j < nplanes/3; j++) {
				for(uint32_t c = 0; c < 3; c++) {
					uint32_t p = j*3 + c;
					line[j][x*3 + c] = material.planes[p].quantize(pri[p]);
				}
			}
		}
	}
}

/*
double RtiBuilder::evaluateError(const std::string &output) {

	Rti decoder;
	if(!decoder.load(output.c_str())) {
		error = decoder.error;
		return 0;
	}
	
	uint32_t size = width*height*3;
	vector<uint8_t> original(size);
	vector<uint8_t> buffer(size);
	
	vector<float> errors(width*height, 0.0f);
	double tot = 0.0;
	for(uint32_t nl = 0; nl < lights.size(); nl++) {
		Vector3f &light = lights[nl];
		decoder.render(light[0], light[1], buffer.data());
		
		imageset.decode(nl, original.data());
		
		double e = 0.0;
		for(uint32_t i = 0; i < width*height; i++) {
			//normalize by luma
			
			
			errors[i] += pow((float)original[i*3+0] - buffer[i*3+0], 2.0f);
			errors[i] += pow((float)original[i*3+1] - buffer[i*3+1], 2.0f);
			errors[i] += pow((float)original[i*3+2] - buffer[i*3+2], 2.0f);
		}
		for(uint32_t i = 0; i < size; i++) {
			double d = (double)original[i] - (double)buffer[i];
			e += d*d;
			//errors[i/3] += d*d;
			//errors[i/3] += d*d;
		}
		
		tot += e/size;
		//cout << "Light: " << nl << " mse: " << sqrt(e/size) << endl;
		
		if(nl == 0) {
			QImage test(buffer.data(), width, height, QImage::Format_RGB888);
			test.save("prova0.png");
			//errorimg.setPixel(x, y, ramp(sqrt(d/3), 0, 50));
		}
	}
	tot /= lights.size();
	double psnr = 20*log10(255.0) - 10*log10(tot);
	
	QImage errorimg(width, height, QImage::Format_RGB32);
	float min = 10.0f; //256.0f;
	float max = 30.0f;
	for(float &e: errors) {
		e = sqrt(e/(lights.size()*3));
		min = std::min(min, e);
		max = std::max(max, e);
	}
	for(uint32_t i = 0; i < width*height; i++)
		errorimg.setPixel(i%width, i/width, ramp(errors[i], min, max));
		
	QDir out(output.c_str());
	errorimg.save(out.filePath("error.png"));
	//cout << "Tot mse: " << sqrt(tot) << " PSNR: " << psnr << endl;
	return sqrt(tot);
} */


/*
PixelArray RtiBuilder::resamplePixels(PixelArray &sample) {

	PixelArray pixels(nsamples, ndimensions);
	
	for(uint32_t i = 0; i < nsamples; i++)
		resamplePixel(sample(i), pixels(i));

	return pixels;
}*/

void RtiBuilder::remapPixel(Pixel  &sample, Pixel &pixel, Resamplemap &resamplemap, float weight) {
	if(weight == 0) return;
	pixel.x = sample.x;
	pixel.y = sample.y;
	for(uint32_t i = 0; i < ndimensions; i++) {
		for(auto &w: resamplemap[i]) {
			pixel[i].r += sample[w.first].r*w.second * weight;
			pixel[i].g += sample[w.first].g*w.second * weight;
			pixel[i].b += sample[w.first].b*w.second * weight;
		}
	}
}

void RtiBuilder::resamplePixel(Pixel &sample, Pixel &pixel) { //pos in pixels.

	pixel.x = sample.x;
	pixel.y = sample.y;
	if(type == BILINEAR) {
		if(imageset.light3d) {
			//TODO move this somewhere else
			float X = (resample_width-1)*sample.x/float(imageset.image_width);
			float Y = (resample_height-1)*sample.y/float(imageset.image_height);

			for(uint32_t i = 0; i < ndimensions; i++)
				pixel[i].r = pixel[i].g = pixel[i].b = 0.0f;

			//find 4 resamplemaps and coefficients
			float ix, iy;
			float dx = modff(X, &ix);
			float dy = modff(Y, &iy);
			Resamplemap &A = resamplemaps[int(ix) + int(iy)*resample_width];
			float wA = (1 - dx)*(1 - dy);
			remapPixel(sample, pixel, A, wA);


			Resamplemap &B = resamplemaps[int(ix+1) + int(iy)*resample_width];
			float wB = dx*(1 - dy);
			remapPixel(sample, pixel, B, wB);

			Resamplemap &C = resamplemaps[int(ix) + int(iy+1)*resample_width];
			float wC = (1 - dx)*dy;
			remapPixel(sample, pixel, C, wC);

			Resamplemap &D = resamplemaps[int(ix+1) + int(iy+1)*resample_width];
			float wD = dx*dy;
			remapPixel(sample, pixel, D, wD);


			//for each pixel we need to know where it is, and compute a resamplemap (will be SLOWWWW)
			//TODO 		it would be faster to compute a few resamplemap in a grid(every 100 pixels or so)
			//		the sample is bilinear interpolation of these key resamplemap
		} else {

			for(uint32_t i = 0; i < ndimensions; i++) {
				pixel[i].r = pixel[i].g = pixel[i].b = 0.0f;
				for(auto &w: resamplemap[i]) {
					pixel[i].r += sample[w.first].r*w.second;
					pixel[i].g += sample[w.first].g*w.second;
					pixel[i].b += sample[w.first].b*w.second;
				}
			}
		}

	} else { //NOT BILINEAR
		for(uint32_t i = 0; i < ndimensions; i++)
			pixel[i] = sample[i];
	}

	for(uint32_t i = 0; i < ndimensions; i++) {
		if(colorspace == MYCC)
			pixel[i] = pixel[i].toYcc();
		else {
			if(gammaFix) {
				pixel[i].r = sqrt(pixel[i].r)*sqrt(255.0f);
				pixel[i].g = sqrt(pixel[i].g)*sqrt(255.0f);
				pixel[i].b = sqrt(pixel[i].b)*sqrt(255.0f);
			}
		}

	}

#ifdef LINEAR
	//RBF interpolation. map a pixel to a sphere pos and interpolate based on euclidean distance
	arma::Col<double> r(lights.size()*3);
	for(int i = 0; i < lights.size(); i++) {
		r[i*3 + 0] = sample[i].r;
		r[i*3 + 1] = sample[i].g;
		r[i*3 + 2] = sample[i].b;
	}
	arma::Col<double> x = arma::solve(H, r);
	for(int i = 0; i < ndimensions; i++) {
		pixel[i].r = std::max(0.0, std::min(255.0, x[i*3+0]));
		pixel[i].g = std::max(0.0, std::min(255.0, x[i*3+1]));
		pixel[i].b = std::max(0.0, std::min(255.0, x[i*3+2]));
	}

	return;
#endif

}

Vector3f RtiBuilder::pixelTo3dCoords(int x, int y) {
	Vector3f pos(
				x/float(resample_width-1),
				y/float(resample_height-1),
				0);
	pos[0] = (pos[0] - imageset.width/2.0f)/imageset.width;
	pos[1] = (pos[1] - imageset.height/2.0f)/imageset.width; //SIC: we work in image coords where width determine the unit.
	return pos;
}

//returno normalized light directions for a pixel
//notice how sample are already intensity corrected.
vector<Vector3f> RtiBuilder::relativeLights(int x, int y) {
	vector<Vector3f> relights = imageset.lights;
	Vector3f pos = pixelTo3dCoords(x, y);
	for(size_t i =	0; i < relights.size(); i++) {
		Vector3f &l = relights[i];
		l = l*2.5f;
		l[0] -= pos[0];
		l[1] -= pos[1];
		l.normalize();
	}
	return relights;
}

void RtiBuilder::buildResampleMaps() {
	if(!imageset.light3d) {
		buildResampleMap(imageset.lights, resamplemap);
		return;
	}
	resample_height = 8;
	resample_width = 8;
	resamplemaps.resize(resample_height*resample_width);

	for(int y = 0; y < resample_height; y++) {
		for(int x = 0; x < resample_width; x++) {
			auto &resamplemap = resamplemaps[x + y*resample_width];
			int pixel_x = imageset.width*x/resample_width;
			int pixel_y = imageset.height*x/resample_height;

			auto relights = relativeLights(pixel_x, pixel_y);
			buildResampleMap(relights, resamplemap);
		}
	}
}


void RtiBuilder::buildResampleMap(std::vector<Vector3f> &lights, std::vector<std::vector<std::pair<int, float>>> &remap) {
	/* every light is linear combination of 4 nearby points (x)
	b = w00x00 + w01x01
	solution is closed form matrix
	b = Ax //H is basically sparse 4 points per line. H is 64 col Time 116 rows.
	//we only want the least square sol.
	invert it:
	x = (At * A)^-1*At*b
	
	*/


	/* To regularize problem expressed above
	 * We want the solution to be close to the rbf approx (skipping cons
	 *
	 * Minimize |Ax - b|^2 + k*|x - x0|^2
	 *
	 * dove x0 = B*b and B is the resulting matrix from rbf.
	 *
	 * Closed form solution is:
	 *
	 * x = x0 + (AtA + kI)^-1 * (At(b - Ax0)
	 * x = B*b + (AtA + kI)^-1 * (At(b - AB*b)
	 * x = B*b + (AtA + kI)^-1 * (At(I - AB)*b)
	 * x = (B + (AtA + kI)^-1 * At*(I - AB))*b
	 */


	float radius = 1/(sigma*sigma);
	Eigen::MatrixXd B = Eigen::MatrixXd::Zero(ndimensions, lights.size());

	remap.resize(ndimensions);
	for(uint32_t y = 0; y < resolution; y++) {
		if(callback) {
			bool keep_going = (*callback)(std::string("Resampling light directions"), 100*y/resolution);
			if(!keep_going) {
				throw std::string("Cancelled.");
			}
		}

		for(uint32_t x = 0; x < resolution; x++) {
			Vector3f n = fromOcta(x, y, resolution);

			//compute rbf weights
			auto &weights = remap[x + y*resolution];
			weights.resize(lights.size());
			float totw = 0.0f;
			for(size_t i = 0; i < lights.size(); i++) {
				float d2 = (n - lights[i]).squaredNorm();
				float w = exp(-radius*d2);
				weights[i] = std::make_pair(i, w);
				totw += w;
			}
			//pick only most significant and renormalize
			float retotw = 0.0f;
			int count = 0;
			for(size_t i = 0; i < weights.size(); i++) {
				float &w = weights[i].second;

				w /= totw;
				if(w > 0.005) { //might fail for extreme smoothing.
					weights[count++] =  weights[i];
					retotw += w;
				}
			}
			weights.resize(count);
			for(auto &i: weights) {
				i.second /= retotw;
				B(x + y*resolution, i.first) = i.second;
			}
		}
	}


	Eigen::MatrixXd A = Eigen::MatrixXd::Zero(lights.size(), ndimensions);
	for(uint32_t l = 0; l < lights.size(); l++) {
		Vector3f &light = lights[l];
		float lx = light[0];
		float ly = light[1];
		float lz = sqrt(1.0f - lx*lx - ly*ly);
		float s = fabs(lx) + fabs(ly) + fabs(lz);

		//rotate 45 deg.
		float x = (lx + ly)/s;
		float y = (ly - lx)/s;

		x = (x + 1.0f)/2.0f;
		y = (y + 1.0f)/2.0f;

		x = x*(resolution - 1.0f);
		y = y*(resolution - 1.0f);

		int sx = std::min(int(resolution-2), std::max(0, int(floor(x))));
		int sy = std::min(int(resolution-2), std::max(0, int(floor(y))));
		float dx = x - float(sx);
		float dy = y - float(sy);

		float s00 = (1 - dx)*(1 - dy);
		float s10 =      dx *(1 - dy);
		float s01 = (1 - dx)* dy;
		float s11 =      dx * dy;

		A(l, ((sx+0) + (sy+0)*resolution)) = double(s00);
		A(l, ((sx+1) + (sy+0)*resolution)) = double(s10);
		A(l, ((sx+0) + (sy+1)*resolution)) = double(s01);
		A(l, ((sx+1) + (sy+1)*resolution)) = double(s11);

	}

	//	 x = (B + (AtA + kI)^-1 * At*(I - AB))*b]
	//  original regularization coefficient was 0.1

	Eigen::MatrixXd I = Eigen::MatrixXd::Identity(ndimensions, ndimensions);
	Eigen::MatrixXd iAtA = (A.transpose()*A  + regularization*I).inverse();
	Eigen::MatrixXd tI = Eigen::MatrixXd::Identity(lights.size(), lights.size());
	Eigen::MatrixXd iA = B + iAtA*(A.transpose() * (tI - A*B));

	remap.clear();
	remap.resize(ndimensions);
	//rows
	for(uint32_t i = 0; i < ndimensions; i++) {
		auto &weights = remap[i];

		//cols
		for(uint32_t c = 0; c < lights.size(); c++) {
			double w = iA(i, c);
			if(fabs(w) > 0.05)
				weights.push_back(std::make_pair(c, w));

		}
	}

	return;
}

std::vector<float> RtiBuilder::toPrincipal(Pixel &pixel) {
	if(!imageset.light3d || type == RBF || type == BILINEAR)
		return toPrincipal(pixel, materialbuilder);

	float ix = (resample_width-1)*pixel.x/float(imageset.image_width);
	float iy = (resample_height-1)*pixel.y/float(imageset.image_height);


	//find 4 resamplemaps and coefficients
	float X, Y;
	float dx = modff(ix, &X);
	float dy = modff(iy, &Y);
	MaterialBuilder &A = materialbuilders[int(X) + int(Y)*resample_width];
	float wA = (1 - dx)*(1 - dy);
	vector<float> res = toPrincipal(pixel, A);


	MaterialBuilder &B = materialbuilders[int(X+1) + int(Y)*resample_width];
	float wB = dx*(1 - dy);
	vector<float> tmp = toPrincipal(pixel, B);
	for(size_t i = 0; i < res.size(); i++)
		res[i] = res[i]*wA + tmp[i]*wB;

	MaterialBuilder &C = materialbuilders[int(X) + int(Y+1)*resample_width];
	float wC = (1 - dx)*dy;
	tmp = toPrincipal(pixel, C);
	for(size_t i = 0; i < res.size(); i++)
		res[i] += tmp[i]*wC;

	MaterialBuilder &D = materialbuilders[int(X+1) + int(Y+1)*resample_width];
	float wD = dx*dy;
	tmp = toPrincipal(pixel, D);
	for(size_t i = 0; i < res.size(); i++)
		res[i] += tmp[i]*wD;
	return res;
}


std::vector<float> RtiBuilder::toPrincipal(Pixel &pixel, MaterialBuilder &materialbuilder) {
	float *v = (float *)pixel.data();
	uint32_t dim = ndimensions*3;

	vector<float> res(nplanes, 0.0f);

	if(colorspace == LRGB) {

		for(size_t p = 0; p < nplanes; p++) {
			for(size_t k = 0; k < dim; k++) {
				res[p] += v[k] * materialbuilder.proj[k + p*dim];
			}
		}

		//get average luminance
		vector<float> luma(ndimensions);
		float max = 0.0;
		for(uint32_t i = 0; i < ndimensions; i++) {
			luma[i] = (0.2125f*v[i*3+0] + 0.7154f*v[i*3+1] + 0.0721f*v[i*3+2])/(255.0f);
			max = std::max(luma[i], max);
		}
		if(max > 0)
			for(float &l: luma)
				l /= max;

		float r = 0.0, g = 0.0, b = 0.0, y = 0.0;
		for(uint32_t i = 0; i < ndimensions; i++) {
			r += (v[i*3+0]/255.0)*(luma[i]);
			g += (v[i*3+1]/255.0)*(luma[i]);
			b += (v[i*3+2]/255.0f)*(luma[i]);
			y += luma[i]*luma[i];
		}
		res[0] = std::max(0.0, std::min(255.0, 255.0*r/y));
		res[1] = std::max(0.0, std::min(255.0, 255.0*g/y));
		res[2] = std::max(0.0, std::min(255.0, 255.0*b/y));

		//		double totluma = (res[0]+ res[1] + res[2])/(3.0 * 255.0);

		double totluma = (0.2125*res[0]+ 0.7154*res[1] + 0.0721*res[2])/255.0;

		for(uint32_t p = 3; p < nplanes; p++)
			res[p] /= totluma;


	} else { //RGB, YCC
		vector<float> col(dim);

		for(size_t k = 0; k < dim; k++)
			col[k] = v[k] - materialbuilder.mean[k];

		for(size_t p = 0; p < nplanes; p++) {
			for(size_t k = 0; k < dim; k++) {
				res[p] += col[k] * materialbuilder.proj[k + p*dim];
			}
		}
		if(colorspace == YCC) {
			int count = 0;
			float cb = 0.0f, cr = 0.0f;
			for(size_t p = 0; p < nplanes; p += 3) {
				Color3f &col = *(Color3f *)&res[p];
				col *= 1/255.0f;
				col = col.RgbToYCbCr();
				if(p < 9) {
					cb += col.g;
					cr += col.b;
					count++;
				}
				if(p > 0)
					col.g = col.b = 0.5f;
				col *= 255.0f;
			}
			res[1] = 255.0f * cb/count;
			res[2] = 255.0f * cr/count;
		}
	}
	return res;
}

