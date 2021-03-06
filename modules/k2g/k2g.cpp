#include "k2g.h"

bool stop = false;

void sigint_handler(int s)
{
    stop = true;
}

K2G::K2G(bool mirror) :
        mirror_(mirror),
        undistorted_(512, 424, 4),
        registered_(512, 424, 4),
        big_mat_(1920, 1082, 4),
        qnan_(std::numeric_limits<float>::quiet_NaN()),
        listener_(libfreenect2::Frame::Color | libfreenect2::Frame::Ir | libfreenect2::Frame::Depth)
{
    //listener_(libfreenect2::Frame::Color | libfreenect2::Frame::Ir | libfreenect2::Frame::Depth),
}

bool K2G::start(Processor p, std::string serial)
{
    signal(SIGINT, sigint_handler);

    if (freenect2_.enumerateDevices() == 0)
    {
        std::cout << "no kinect2 connected!" << std::endl;
        return false;
    }

    switch (p)
    {
        case CPU:
            std::cout << "creating Cpu processor" << std::endl;
            if (serial.empty())
                dev_ = freenect2_.openDefaultDevice(new libfreenect2::CpuPacketPipeline());
            else
                dev_ = freenect2_.openDevice(serial, new libfreenect2::CpuPacketPipeline());
            std::cout << "created" << std::endl;
            break;
#ifdef WITH_OPENCL
        case OPENCL:
            std::cout << "creating OpenCL processor" << std::endl;
            if(serial.empty())
                dev_ = freenect2_.openDefaultDevice(new libfreenect2::OpenCLPacketPipeline());
            else
                dev_ = freenect2_.openDevice(serial, new libfreenect2::OpenCLPacketPipeline());
            break;
#endif
        case OPENGL:
            std::cout << "creating OpenGL processor" << std::endl;
            if (serial.empty())
                dev_ = freenect2_.openDefaultDevice(new libfreenect2::OpenGLPacketPipeline());
            else
                dev_ = freenect2_.openDevice(serial, new libfreenect2::OpenGLPacketPipeline());
            break;
#ifdef WITH_CUDA
        case CUDA:
            std::cout << "creating Cuda processor" << std::endl;
            if(serial.empty())
                dev_ = freenect2_.openDefaultDevice(new libfreenect2::CudaPacketPipeline());
            else
                dev_ = freenect2_.openDevice(serial, new libfreenect2::CudaPacketPipeline());
            break;
#endif
        default:
            std::cout << "creating Cpu processor" << std::endl;
            if (serial_.empty())
                dev_ = freenect2_.openDefaultDevice(new libfreenect2::CpuPacketPipeline());
            else
                dev_ = freenect2_.openDevice(serial, new libfreenect2::CpuPacketPipeline());
            break;
    }

    if (!serial.empty())
        serial_ = serial;
    else
        serial_ = freenect2_.getDefaultDeviceSerialNumber();

    dev_->setColorFrameListener(&listener_);
    dev_->setIrAndDepthFrameListener(&listener_);
    dev_->start();

    logger_ = libfreenect2::getGlobalLogger();

    registration_ = new libfreenect2::Registration(dev_->getIrCameraParams(), dev_->getColorCameraParams());

    prepareMake3D(dev_->getIrCameraParams());
#ifdef WITH_SERIALIZATION
    serialize_ = false;
    file_streamer_ = NULL;
    oa_ = NULL;
#endif
    return true;
}

bool K2G::stop()
{
    if (dev_ != nullptr)
    {
        dev_->stop();
        dev_->close();
        delete dev_;
        delete registration_;
    }
}


void K2G::printParameters()
{
    libfreenect2::Freenect2Device::ColorCameraParams cp = getRgbParameters();
    std::cout << "rgb fx=" << cp.fx << ",fy=" << cp.fy <<
              ",cx=" << cp.cx << ",cy=" << cp.cy << std::endl;
    libfreenect2::Freenect2Device::IrCameraParams ip = getIrParameters();
    std::cout << "ir fx=" << ip.fx << ",fy=" << ip.fy <<
              ",cx=" << ip.cx << ",cy=" << ip.cy <<
              ",k1=" << ip.k1 << ",k2=" << ip.k2 << ",k3=" << ip.k3 <<
              ",p1=" << ip.p1 << ",p2=" << ip.p2 << std::endl;
}

void K2G::storeParameters()
{
    libfreenect2::Freenect2Device::ColorCameraParams cp = getRgbParameters();
    libfreenect2::Freenect2Device::IrCameraParams ip = getIrParameters();

    cv::Mat rgb = (cv::Mat_<float>(3, 3) << cp.fx, 0, cp.cx, 0, cp.fy, cp.cy, 0, 0, 1);
    cv::Mat depth = (cv::Mat_<float>(3, 3) << ip.fx, 0, ip.cx, 0, ip.fy, ip.cy, 0, 0, 1);
    cv::Mat depth_dist = (cv::Mat_<float>(1, 5) << ip.k1, ip.k2, ip.p1, ip.p2, ip.k3);
    std::cout << "storing " << serial_ << std::endl;
    cv::FileStorage fs("calib_" + serial_ + ".yml", cv::FileStorage::WRITE);

    fs << "CcameraMatrix" << rgb;
    fs << "DcameraMatrix" << depth << "distCoeffs" << depth_dist;

    fs.release();

}

#ifdef WITH_PCL

pcl::PointCloud<pcl::PointXYZRGBA>::Ptr K2G::getCloud()
{
    const short w = undistorted_.width;
    const short h = undistorted_.height;
    pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud(new pcl::PointCloud<pcl::PointXYZRGBA>(w, h));

    return updateCloud(cloud);
}

pcl::PointCloud<pcl::PointXYZRGBA>::Ptr K2G::getCloud(const libfreenect2::Frame *rgb, const libfreenect2::Frame *depth,
                                                      pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud)
{
    return updateCloud(rgb, depth, cloud);
}

pcl::PointCloud<pcl::PointXYZRGBA>::Ptr K2G::updateCloud(pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud)
{

    listener_.waitForNewFrame(frames_);
    libfreenect2::Frame *rgb = frames_[libfreenect2::Frame::Color];
    libfreenect2::Frame *depth = frames_[libfreenect2::Frame::Depth];

    registration_->apply(rgb, depth, &undistorted_, &registered_, true, &big_mat_, map_);
    const std::size_t w = undistorted_.width;
    const std::size_t h = undistorted_.height;

    cv::Mat tmp_itD0(undistorted_.height, undistorted_.width, CV_8UC4, undistorted_.data);
    cv::Mat tmp_itRGB0(registered_.height, registered_.width, CV_8UC4, registered_.data);

    if (mirror_ == true)
    {

        cv::flip(tmp_itD0, tmp_itD0, 1);
        cv::flip(tmp_itRGB0, tmp_itRGB0, 1);

    }

    const float *itD0 = (float *) tmp_itD0.ptr();
    const char *itRGB0 = (char *) tmp_itRGB0.ptr();

    pcl::PointXYZRGBA *itP = &cloud->points[0];
    bool is_dense = true;

    for (std::size_t y = 0; y < h; ++y)
    {

        const unsigned int offset = y * w;
        const float *itD = itD0 + offset;
        const char *itRGB = itRGB0 + offset * 4;
        const float dy = rowmap(y);

        for (std::size_t x = 0; x < w; ++x, ++itP, ++itD, itRGB += 4)
        {
            const float depth_value = *itD / 1000.0f;

            if (!std::isnan(depth_value) && !(std::abs(depth_value) < 0.0001))
            {

                const float rx = colmap(x) * depth_value;
                const float ry = dy * depth_value;
                itP->z = depth_value;
                itP->x = rx;
                itP->y = ry;

                itP->b = itRGB[0];
                itP->g = itRGB[1];
                itP->r = itRGB[2];
                itP->a = 255;
            } else
            {
                itP->z = qnan_;
                itP->x = qnan_;
                itP->y = qnan_;

                itP->b = qnan_;
                itP->g = qnan_;
                itP->r = qnan_;
                is_dense = false;
            }
        }
    }
    cloud->is_dense = is_dense;
    listener_.release(frames_);
#ifdef WITH_SERIALIZATION
    if (serialize_)
        serializeCloud(cloud);
#endif
    return cloud;
}

pcl::PointCloud<pcl::PointXYZRGBA>::Ptr
K2G::updateCloud(const libfreenect2::Frame *rgb, const libfreenect2::Frame *depth,
                 pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud)
{

//     registration_->apply(rgb, depth, &undistorted_, &registered_, true, &big_mat_, map_);

    const std::size_t w = undistorted_.width;
    const std::size_t h = undistorted_.height;

    if (cloud->size() != w * h)
        cloud->resize(w * h);

    cv::Mat tmp_itD0(undistorted_.height, undistorted_.width, CV_8UC4, undistorted_.data);
    cv::Mat tmp_itRGB0(registered_.height, registered_.width, CV_8UC4, registered_.data);

    if (mirror_ == true)
    {

        cv::flip(tmp_itD0, tmp_itD0, 1);
        cv::flip(tmp_itRGB0, tmp_itRGB0, 1);

    }

    const float *itD0 = (float *) tmp_itD0.ptr();
    const char *itRGB0 = (char *) tmp_itRGB0.ptr();

    pcl::PointXYZRGBA *itP = &cloud->points[0];
    bool is_dense = true;

#pragma omp parallel for
    for (std::size_t y = 0; y < h; ++y)
    {

        const unsigned int offset = y * w;
        const float *itD = itD0 + offset;
        const char *itRGB = itRGB0 + offset * 4;
        const float dy = rowmap(y);

        pcl::PointXYZRGBA *itPc = itP + offset;

        for (std::size_t x = 0; x < w; ++x, ++itPc, ++itD, itRGB += 4)
        {
            const float depth_value = *itD / 1000.0f;

            if (!std::isnan(depth_value) && !(std::abs(depth_value) < 0.0001))
            {

                const float rx = colmap(x) * depth_value;
                const float ry = dy * depth_value;
                itPc->z = depth_value;
                itPc->x = rx;
                itPc->y = ry;

                itPc->b = itRGB[0];
                itPc->g = itRGB[1];
                itPc->r = itRGB[2];
                itPc->a = 255;
            } else
            {
                itPc->z = qnan_;
                itPc->x = qnan_;
                itPc->y = qnan_;

                itPc->b = qnan_;
                itPc->g = qnan_;
                itPc->r = qnan_;
                is_dense = false;
            }
        }
    }
    cloud->is_dense = is_dense;
#ifdef WITH_SERIALIZATION
    if (serialize_)
        serializeCloud(cloud);
#endif
    return cloud;
}

#endif

void K2G::getDepth(cv::Mat depth_mat)
{
    listener_.waitForNewFrame(frames_);
    libfreenect2::Frame *depth = frames_[libfreenect2::Frame::Depth];

    cv::Mat depth_tmp(depth->height, depth->width, CV_32FC1, depth->data);

    if (mirror_ == true)
    {
        cv::flip(depth_tmp, depth_mat, 1);
    } else
    {
        depth_mat = depth_tmp.clone();
    }
    listener_.release(frames_);
}

void K2G::getIr(cv::Mat ir_mat)
{
    listener_.waitForNewFrame(frames_);
    libfreenect2::Frame *ir = frames_[libfreenect2::Frame::Ir];

    cv::Mat ir_tmp(ir->height, ir->width, CV_32FC1, ir->data);

    if (mirror_ == true)
    {
        cv::flip(ir_tmp, ir_mat, 1);
    } else
    {
        ir_mat = ir_tmp.clone();
    }
    listener_.release(frames_);
}

// Use only if you want only color, else use get(cv::Mat, cv::Mat) to have the images aligned
void K2G::getColor(cv::Mat &color_mat)
{
    listener_.waitForNewFrame(frames_);
    libfreenect2::Frame *rgb = frames_[libfreenect2::Frame::Color];

    cv::Mat tmp_color(rgb->height, rgb->width, CV_8UC4, rgb->data);

    if (mirror_ == true)
    {
        cv::flip(tmp_color, color_mat, 1);
    } else
    {
        color_mat = tmp_color.clone();
    }
    listener_.release(frames_);
}

// Depth and color are aligned and registered 
void K2G::get(cv::Mat &color_mat, cv::Mat &depth_mat, const bool full_hd, const bool remove_points)
{
    listener_.waitForNewFrame(frames_);
    libfreenect2::Frame *rgb = frames_[libfreenect2::Frame::Color];
    libfreenect2::Frame *depth = frames_[libfreenect2::Frame::Depth];

    registration_->apply(rgb, depth, &undistorted_, &registered_, remove_points, &big_mat_, map_);

    cv::Mat tmp_depth(undistorted_.height, undistorted_.width, CV_32FC1, undistorted_.data);
    cv::Mat tmp_color;
    if (full_hd)
        tmp_color = cv::Mat(rgb->height, rgb->width, CV_8UC4, rgb->data);
    else
        tmp_color = cv::Mat(registered_.height, registered_.width, CV_8UC4, registered_.data);

    if (mirror_ == true)
    {
        cv::flip(tmp_depth, depth_mat, 1);
        cv::flip(tmp_color, color_mat, 1);
    } else
    {
        color_mat = tmp_color.clone();
        depth_mat = tmp_depth.clone();
    }

    listener_.release(frames_);
}


// Depth and color are aligned and registered 
void K2G::get(cv::Mat &color_mat, cv::Mat &depth_mat, cv::Mat &ir_mat, const bool full_hd, const bool remove_points)
{
    listener_.waitForNewFrame(frames_);
    libfreenect2::Frame *rgb = frames_[libfreenect2::Frame::Color];
    libfreenect2::Frame *depth = frames_[libfreenect2::Frame::Depth];
    libfreenect2::Frame *ir = frames_[libfreenect2::Frame::Ir];

    registration_->apply(rgb, depth, &undistorted_, &registered_, remove_points, &big_mat_, map_);

    cv::Mat tmp_depth(undistorted_.height, undistorted_.width, CV_32FC1, undistorted_.data);
    cv::Mat tmp_color;
    cv::Mat ir_tmp(ir->height, ir->width, CV_32FC1, ir->data);

    if (full_hd)
        tmp_color = cv::Mat(rgb->height, rgb->width, CV_8UC4, rgb->data);
    else
        tmp_color = cv::Mat(registered_.height, registered_.width, CV_8UC4, registered_.data);

    if (mirror_ == true)
    {
        cv::flip(tmp_depth, depth_mat, 1);
        cv::flip(tmp_color, color_mat, 1);
        cv::flip(ir_tmp, ir_mat, 1);
    } else
    {
        color_mat = tmp_color.clone();
        depth_mat = tmp_depth.clone();
        ir_mat = ir_tmp.clone();
    }

    listener_.release(frames_);
}

#ifdef WITH_PCL

// All frame and cloud are aligned. There is a small overhead in the double call to registration->apply which has to be removed
void K2G::get(cv::Mat &color_mat, cv::Mat &depth_mat, pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud, const bool full_hd,
              const bool remove_points)
{
    listener_.waitForNewFrame(frames_);
    libfreenect2::Frame *rgb = frames_[libfreenect2::Frame::Color];
    libfreenect2::Frame *depth = frames_[libfreenect2::Frame::Depth];

    registration_->apply(rgb, depth, &undistorted_, &registered_, remove_points, &big_mat_, map_);

    cv::Mat tmp_depth(undistorted_.height, undistorted_.width, CV_32FC1, undistorted_.data);
    cv::Mat tmp_color;

    if (full_hd)
        tmp_color = cv::Mat(rgb->height, rgb->width, CV_8UC4, rgb->data);
    else
        tmp_color = cv::Mat(registered_.height, registered_.width, CV_8UC4, registered_.data);

    if (mirror_ == true)
    {
        cv::flip(tmp_depth, depth_mat, 1);
        cv::flip(tmp_color, color_mat, 1);
    } else
    {
        color_mat = tmp_color.clone();
        depth_mat = tmp_depth.clone();
    }

    cloud = getCloud(rgb, depth, cloud);
    listener_.release(frames_);
}

#endif

#ifdef WITH_SERIALIZATION

void K2G::serializeFrames(const cv::Mat &depth, const cv::Mat &color)
{
    std::chrono::high_resolution_clock::time_point tnow = std::chrono::high_resolution_clock::now();
    unsigned int now = (unsigned int) std::chrono::duration_cast<std::chrono::milliseconds>(
            tnow.time_since_epoch()).count();
    if (!file_streamer_)
    {
        file_streamer_ = new std::ofstream();
        file_streamer_->open("stream" + std::to_string(now), std::ios::binary);
        oa_ = new boost::archive::binary_oarchive(*file_streamer_);
    }

    (*oa_) << now << color;
}

#ifdef WITH_PCL

void K2G::serializeCloud(const pcl::PointCloud<pcl::PointXYZRGBA>::Ptr cloud)
{
    std::chrono::high_resolution_clock::time_point tnow = std::chrono::high_resolution_clock::now();
    unsigned int now = (unsigned int) std::chrono::duration_cast<std::chrono::milliseconds>(
            tnow.time_since_epoch()).count();
    if (!file_streamer_)
    {
        std::cout << "opening stream" << std::endl;
        file_streamer_ = new std::ofstream();
        file_streamer_->open("stream" + std::to_string(now), std::ios::binary);
    }

    microser sr(*file_streamer_);
    sr << now << (uint32_t) cloud->size();
    for (auto &p : cloud->points)
    {
        sr << p.x << p.y << p.z << p.r << p.g << p.b;
    }
}

#endif
#endif

void K2G::prepareMake3D(const libfreenect2::Freenect2Device::IrCameraParams &depth_p)
{
    const int w = 512;
    const int h = 424;
    float *pm1 = colmap.data();
    float *pm2 = rowmap.data();
    for (int i = 0; i < w; i++)
    {
        *pm1++ = (i - depth_p.cx + 0.5) / depth_p.fx;
    }
    for (int i = 0; i < h; i++)
    {
        *pm2++ = (i - depth_p.cy + 0.5) / depth_p.fy;
    }
}