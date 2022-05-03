#ifndef DSTRETCH_H
#define DSTRETCH_H

#include <QFile>
#include <imageset.h>
#include "jpeg_decoder.h"
#include "jpeg_encoder.h"
#include <Eigen/Eigen>
#include <Eigen/Eigenvalues>
#include <QString>
#include <QDebug>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>

/** info.json:
 *  - Nome del file
 *  - Matrice di trasformazione
 *  - samples
 */

inline void dstretchSingle(QString fileName, QString output, int minSamples, std::function<bool(std::string s, int n)> progressed)
{
    // info.json
    QFile info(output.mid(0, output.lastIndexOf("/")) + "/dstretch_info.json");
    QJsonObject infoObject;
    QJsonArray samplesJson;
    QJsonArray transformJson;
    QJsonDocument outputJson;
    // Vector used to temporarily store a line of pixels
    std::vector<uint8_t> pixels;
    // Vector used to store samples
    std::vector<Color3b> samples;

    // Final data to be saved
    std::vector<int> dstretched;
    std::vector<uint8_t> dstretchedBytes;
    // Jpeg encoder and decoder to handle output and input
    JpegDecoder decoder;
    JpegEncoder encoder;

    // Size of the image
    int width, height;
    // Covariance matrix
    Eigen::MatrixXd covariance(3, 3);
    // Channel means
    Eigen::VectorXd means(3);

    // Max and min values for channels (used to rescale the output)
    int mins[] = {256, 256, 256};
    int maxs[] = {-1, -1, -1};

    // Initialization
    decoder.init(fileName.toStdString().c_str(), width, height);
    encoder.init(output.toStdString().c_str(), width, height);
    pixels.resize(width * 3);

    means.fill(0);

    // Compute the distances between a sample and another one so that at least minSamples are taken
    uint32_t samplesHorizontal = std::ceil(std::sqrt(minSamples) * ((float)width / height));
    uint32_t samplesVertical =  std::ceil(std::sqrt(minSamples) * ((float)height / width));
    uint32_t rowSkip = std::max<uint32_t>(1, height / samplesVertical);
    uint32_t colSkip = std::max<uint32_t>(1, width / samplesHorizontal);

    // Sample each line
    for (int row=0; row<height; row++)
    {
        // Read the line
        decoder.readRows(1, pixels.data());

        if (row % rowSkip == 0)
        {
            // Getting the samples
            for (int col=0; col<pixels.size(); col+=colSkip*3)
            {
                for (int i=0; i<3; i++)
                    means(i) += pixels[col + i];
                Color3b color(pixels[col], pixels[col+1], pixels[col+2]);

                samples.push_back(color);
                samplesJson.push_back(QJsonArray({color.r, color.b, color.g}));
            }
        }

        if (row % 10 == 0)
            progressed("Sampling...", (row * 100) / width);
    }

    // Compute the mean of the channels
    for (int i=0; i<3; i++)
        means(i) /= samples.size();

    // Compute the sums needed to compute the covariance
    long sumChannel[]= {0,0,0};
    double sumX[][3] = {{0,0,0},{0,0,0},{0,0,0}};

    for (int k=0; k<samples.size(); k++)
        for (int i=0; i<3; i++)
            sumChannel[i] += samples[k][i];

    for (int l=0; l<3; l++)
        for (int m=0; m<3; m++)
            for (int k=0; k<samples.size(); k++)
                sumX[l][m] += samples[k][l] * samples[k][m];

    // Compute the covariance
    for (int l=0; l<3; l++)
        for (int m=0; m<3; m++)
            covariance(l,m) = ((double)(1.0f/(samples.size() - 1))) * (sumX[l][m] - ((double)1.0f/samples.size())*sumChannel[l]*sumChannel[m]);

    // Compute the rotation
    Eigen::EigenSolver<Eigen::MatrixXd> solver(covariance, true);
    Eigen::MatrixXd rotation = solver.eigenvectors().real();
    Eigen::MatrixXd eigenValues = solver.eigenvalues().real();

    Eigen::MatrixXd sigma = covariance.diagonal().asDiagonal();
    for (int i=0; i<3; i++)
        sigma(i, i) = std::sqrt(sigma(i,i));

    // Compute the stretching factor
    for (int i=0; i<3; i++)
        eigenValues(i) = 1.0f / std::sqrt(eigenValues(i) >= 0 ? eigenValues(i) : -eigenValues(i));

    // Compute the final transformation matrix
    Eigen::MatrixXd transformation = sigma * rotation * eigenValues.asDiagonal() * rotation.transpose();
    // Apply the transformation to the mean
    Eigen::VectorXd offset = means - transformation * means;

    // Finally reposition the pixels with that offset
    decoder.init(fileName.toStdString().c_str(), width, height);
    Eigen::VectorXd currPixel(3);

    for (int i=0; i<height; i++)
    {
        decoder.readRows(1, pixels.data());
        for (int k=0; k<pixels.size(); k+=3)
        {
            for (int j=0; j<3; j++)
                currPixel(j) = pixels[k+j];

            currPixel -= means;
            currPixel = transformation * currPixel + means + offset;

            for (int j=0; j<3; j++)
            {
                dstretched.push_back(currPixel[j]);
                mins[j] = std::min<int>(mins[j], currPixel[j]);
                maxs[j] = std::max<int>(maxs[j], currPixel[j]);
            }
        }

        if (i % 100 == 0)
            progressed("Transforming...", (i * 100) / height);
    }

    for (int k=0; k<dstretched.size(); k++)
    {
        uint32_t channelIdx = k % 3;
        dstretchedBytes.push_back(255 * ((float)(dstretched[k] - mins[channelIdx]) / (maxs[channelIdx] - mins[channelIdx])));

        if (k % 100 == 0)
            progressed("Scaling...", (k * 100) / dstretched.size());
    }

    progressed("Saving...", 50);
    encoder.writeRows(dstretchedBytes.data(), height);
    encoder.finish();

    // Prepare and save the info.json file
    info.open(QIODevice::WriteOnly);
    info.setPermissions(QFileDevice::WriteOther | QFileDevice::WriteOwner);
    infoObject["image_name"] = fileName.mid(fileName.lastIndexOf("/")+1, fileName.size());
    for (int i=0; i<3; i++)
        for (int j=0; j<3; j++)
            transformJson.push_back(transformation(i,j));
    infoObject["transformation"] = transformJson;
    infoObject["samples"] = samplesJson;

    outputJson.setObject(infoObject);
    info.write(outputJson.toJson());
    info.close();
}


inline void dstretchSet(QString inputFolder, QString output, int minSamples, std::function<bool(std::string s, int n)> progressed)
{
    output = output.mid(0, output.lastIndexOf("/"));
    qDebug() << "SETT";
    ImageSet set;
    // Vector used to temporarily store a line of pixels
    PixelArray pixels;
    // Vector used to store samples
    std::vector<Color3f> samples;

    // Final data to be saved
    std::vector<std::vector<float>> dstretched;
    std::vector<std::vector<uint8_t>> dstretchedBytes;

    // Size of the image
    int width, height;
    // Covariance matrix
    Eigen::MatrixXd covariance(3, 3);
    // Channel means
    Eigen::VectorXd means(3);

    // Max and min values for channels (used to rescale the output)
    float mins[] = {2048, 2048, 2048}, maxs[] = {-2048,-2048,-2048};

    set.setCallback(nullptr);
    set.initFromFolder(inputFolder.toStdString().c_str());

    width = set.width;
    height = set.height;
    pixels.resize(width, set.images.size());
    means.fill(0);

    dstretched.resize(set.images.size());
    dstretchedBytes.resize(set.images.size());

    // Compute the distances between a sample and another one so that at least minSamples are taken
    uint32_t samplesHorizontal = std::ceil(std::sqrt(minSamples) * ((float)width / height));
    uint32_t samplesVertical =  std::ceil(std::sqrt(minSamples) * ((float)height / width));
    uint32_t rowSkip = std::max<uint32_t>(1, height / samplesVertical);
    uint32_t colSkip = std::max<uint32_t>(1, width / samplesHorizontal);

    // Sample each line
    for (int row=0; row<height; row++)
    {
        // Read the line
        set.readLine(pixels);

        if (row % rowSkip == 0)
        {
            // Getting the samples
            for (int col=0; col<pixels.size(); col+=colSkip*3)
            {
                for (int l=0; l<pixels.nlights; l++)
                    for (int i=0; i<3; i++)
                        // For each light we have a line, take the col pixel from that line and add the i channel
                        means(i) += pixels[col][l][i];

                for (int l=0; l<pixels.nlights; l++)
                    samples.push_back(pixels[col][l]);
            }
        }

        if (row % 10 == 0)
            progressed("Sampling...", (row * 100) / width);
    }

    // Compute the mean of the channels
    for (int i=0; i<3; i++)
        means(i) /= samples.size();

    // Compute the sums needed to compute the covariance
    long sumChannel[]= {0,0,0};
    double sumX[][3] = {{0,0,0},{0,0,0},{0,0,0}};

    for (int k=0; k<samples.size(); k++)
        for (int i=0; i<3; i++)
            sumChannel[i] += samples[k][i];

    for (int l=0; l<3; l++)
        for (int m=0; m<3; m++)
            for (int k=0; k<samples.size(); k++)
                sumX[l][m] += samples[k][l] * samples[k][m];

    // Compute the covariance
    for (int l=0; l<3; l++)
        for (int m=0; m<3; m++)
            covariance(l,m) = ((double)(1.0f/(samples.size() - 1))) * (sumX[l][m] - ((double)1.0f/samples.size())*sumChannel[l]*sumChannel[m]);

    // Compute the rotation
    Eigen::EigenSolver<Eigen::MatrixXd> solver(covariance, true);
    Eigen::MatrixXd rotation = solver.eigenvectors().real();
    Eigen::MatrixXd eigenValues = solver.eigenvalues().real();

    Eigen::MatrixXd sigma = covariance.diagonal().asDiagonal();
    for (int i=0; i<3; i++)
        sigma(i, i) = std::sqrt(sigma(i,i));

    // Compute the stretching factor
    for (int i=0; i<3; i++)
        eigenValues(i) = 1.0f / std::sqrt(eigenValues(i) >= 0 ? eigenValues(i) : -eigenValues(i));

    // Compute the final transformation matrix
    Eigen::MatrixXd transformation = sigma * rotation * eigenValues.asDiagonal() * rotation.transpose();
    // Apply the transformation to the mean
    Eigen::VectorXd offset = means - transformation * means;

    // Start up the encoders
    JpegEncoder* encoders = new JpegEncoder[set.images.size()];

    // Transform and scale all the images in different files
    Eigen::VectorXd currPixel(3);
    PixelArray line;
    set.restart();

    for (int i=0; i<height; i++)
    {
        // Read a line
        set.readLine(line);

        // Transform all the pixels of all the images, store the result in a vector tied to the right image
        for (int im=0; im<set.images.size(); im++)
        {
            for (int k=0; k<line.size(); k++)
            {
                for (int j=0; j<3; j++)
                    currPixel(j) = line[k][im][j];

                currPixel -= means;
                currPixel = transformation * currPixel/* + means + offset*/;

                for (int j=0; j<3; j++)
                {
                    dstretched[im].push_back(currPixel[j]);
                    mins[j] = std::min<int>(mins[j], currPixel[j]);
                    maxs[j] = std::max<int>(maxs[j], currPixel[j]);
                }
            }
        }

        if (i % 10 == 0)
            progressed("Transforming...", (i * 100) / height);
    }

    qDebug() << "Out files: " << QString(output + "/img_%1.jpg").arg(1).toStdString().c_str();

    for (int im=0; im<set.images.size(); im++)
    {
        for (int k=0; k<dstretched[im].size(); k++)
        {
            uint32_t channelIdx = k % 3;
            dstretchedBytes[im].push_back(255 * ((float)(dstretched[im][k] - mins[channelIdx]) / (maxs[channelIdx] - mins[channelIdx])));
        }

        encoders[im].encode(dstretchedBytes[im].data(), width, height, (inputFolder + "/" + set.images[im]).toStdString().c_str());
        progressed("Scaling...", (im * 100) / set.images.size());
    }

    // TODO resize instead of push_back
}

#endif // DSTRETCH_H

