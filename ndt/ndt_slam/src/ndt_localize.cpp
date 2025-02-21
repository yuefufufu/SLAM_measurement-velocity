//==================================================================
// Name         : ndt_localize.cpp
// Brief        : perform SLAM
// Author       : Yasuaki ORITA
// Time-Stamp   : <2020/02/29 23:41>
//
// Design for IVeSlab rescue project
//==================================================================

#include <iostream>
#include <sstream>
#include <fstream>
#include <string>

#include <ros/ros.h>
#include <std_msgs/Bool.h>
#include <std_msgs/Float32.h>
#include <sensor_msgs/PointCloud2.h>
#include <velodyne_pointcloud/rawdata.h>

#include <tf/transform_broadcaster.h>
#include <tf/transform_datatypes.h>

#include <pcl/io/io.h>
#include <pcl/io/pcd_io.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
// 多分これがないか名前かわる？

//#ifdef USE_FAST_PCL
#include <fast_pcl/registration/ndt.h>
#include <fast_pcl/filters/voxel_grid.h>
//#else
//#include <pcl/registration/ndt.h>
//#include <pcl/filters/voxel_grid.h>
//#endif

#include <geometry_msgs/PointStamped.h>



struct pose { 
   double x;
   double y;
   double z;
   double roll;
   double pitch;
   double yaw;
};

double robot_v;
double robot_w;
double robot_pre_v;
std_msgs::Float32 robot_pub_v;
ros::Publisher v_pub;

//グローバル変数
static pose current_pose, previous_pose, guess_pose, ndt_pose, added_pose, localizer_pose;
static double offset_x, offset_y, offset_z, offset_yaw;

static pcl::PointCloud<pcl::PointXYZI> map;
static pcl::NormalDistributionsTransform<pcl::PointXYZI, pcl::PointXYZI> ndt;

//ニュートン法におけるデフォルト値
static int iter = 30; // ニュートン法における繰返し回数の上限
static float ndt_res = 1.0; // Resolution
static double step_size = 0.1; // Step size
static double trans_eps = 0.01; // 収束判定値
static double fitness_score;

//voxelgridfilterの設定
static double voxel_leaf_size = 1.0;//入力スキャンに対するフィルタ
static double voxel_leaf_size1 = 0.20;//地図データ登録時のフィルタ，登録点群数が大きくなり過ぎないように
// static double voxel_leaf_size2 = 0.20;//地図データに対するフィルタ

// //Publisher関連
// static ros::Publisher ndt_map_pub;

// static ros::Publisher slam_pub;

//mapの初期設定
static int initial_scan_loaded = 0;

//launchファイルから値を取得
static double RANGE = 0.0;
static double SHIFT = 0.0;

//velodyneと制御位置の補正用
static double _tf_x = 0.0;
static double _tf_y = 0.0;
static double _tf_z = 0.0;
static double _tf_roll = 0.0;
static double _tf_pitch = 0.0;
static double _tf_yaw = 0.0;
static Eigen::Matrix4f tf_btol, tf_ltob;

static bool isMapUpdate = true;
static bool _use_openmp = false;

// ros::Publisher current_point_pub;
geometry_msgs::PointStamped current_point;

int slam_time = 0;


void slam_data_save()
{
  std::ofstream ofs("/home/rescue-nuc/data/slam/slam.dat", std::ios::out | std::ios::app);
  ofs << current_pose.x << " " << current_pose.y << " " << current_pose.z << " " << current_pose.roll << " " << current_pose.pitch << " " << current_pose.yaw << std::endl;
  ofs.close();
}


    
static void points_callback(const sensor_msgs::PointCloud2::ConstPtr& input)
{
    double r;
    pcl::PointXYZI p;
    pcl::PointCloud<pcl::PointXYZI> tmp, scan;
    pcl::PointCloud<pcl::PointXYZI>::Ptr filtered_scan_ptr (new pcl::PointCloud<pcl::PointXYZI>());
    pcl::PointCloud<pcl::PointXYZI>::Ptr transformed_scan_ptr (new pcl::PointCloud<pcl::PointXYZI>());
    tf::Quaternion q;

    Eigen::Matrix4f t_localizer(Eigen::Matrix4f::Identity());
    Eigen::Matrix4f t_base_link(Eigen::Matrix4f::Identity());
    tf::TransformBroadcaster br;
    tf::Transform transform;

    ros::Time scan_time = input->header.stamp;

    pcl::fromROSMsg(*input, tmp); //PointCloud2->PointCloud<PointT>

    ros::Time callback_start = ros::Time::now();
	
    for (pcl::PointCloud<pcl::PointXYZI>::const_iterator item = tmp.begin(); item != tmp.end(); item++){
    	p.x = (double) item->x;
    	p.y = (double) item->y;
    	p.z = (double) item->z;
    	p.intensity = (double) item->intensity;

    	r = sqrt(pow(p.x, 2.0) + pow(p.y, 2.0));
    	if(r > RANGE){
    		scan.push_back(p);
    	}
    }

    pcl::PointCloud<pcl::PointXYZI>::Ptr scan_ptr(new pcl::PointCloud<pcl::PointXYZI>(scan));
    
    // mapに1フレーム目を追加
    if(initial_scan_loaded == 0){
      map += *scan_ptr;
      initial_scan_loaded = 1;
    }
    
    pcl::PointCloud<pcl::PointXYZI>::Ptr map_ptr(new pcl::PointCloud<pcl::PointXYZI>(map));

    // scanにvoxelgridfilterを適用
    pcl::VoxelGrid<pcl::PointXYZI> voxel_grid_filter;
    voxel_grid_filter.setLeafSize(voxel_leaf_size, voxel_leaf_size, voxel_leaf_size);
    voxel_grid_filter.setInputCloud(scan_ptr);
    voxel_grid_filter.filter(*filtered_scan_ptr);
	
    //位置合わせの設定
    ndt.setTransformationEpsilon(trans_eps);
    ndt.setStepSize(step_size);
    ndt.setResolution(ndt_res);
    ndt.setMaximumIterations(iter);
    ndt.setInputSource(filtered_scan_ptr);

    // if(slam_switch == 0){
    //   if(isMapUpdate == true){ //shift(移動距離)に関係
    //   ndt.setInputTarget(map_ptr);
    //   isMapUpdate = false;
    // }
    // }

    //added
    //if(slam_switch == 1){
    pcl::PointXYZI  map_p;
    //pcl::PointCloud<pcl::PointXYZI>::Ptr limited_map_ptr(new pcl::PointCloud<pcl::PointXYZI>());   
    pcl::PointCloud<pcl::PointXYZI> limited_map;
    limited_map.header.frame_id = "map";
    if(isMapUpdate == true){ //shift(移動距離)に関係	  
      for (pcl::PointCloud<pcl::PointXYZI>::const_iterator map_item = map.begin(); map_item != map.end(); map_item++){
	map_p.x = (double) map_item->x;
	map_p.y = (double) map_item->y;
	map_p.z = (double) map_item->z;
	map_p.intensity = (double) map_item->intensity;
	
	if(map_p.x > (current_pose.x - 10.0) && map_p.x < (current_pose.x + 10.0) &&
	   map_p.y > (current_pose.y - 10.0) && map_p.y < (current_pose.y + 10.0)){
	  limited_map.push_back(map_p);
	}
      }
      std::cout << "limited_map: " << limited_map.points.size() << " points." << std::endl;
      map = limited_map;  //議論の余地あり
      ndt.setInputTarget(map_ptr);
      isMapUpdate = false;
    }
    //}

    //過去の情報から現在の位置を推測(guess)する．収束を早めるため
    guess_pose.x = previous_pose.x + offset_x;
    guess_pose.y = previous_pose.y + offset_y;
    guess_pose.z = previous_pose.z + offset_z;
    guess_pose.roll = previous_pose.roll;
    guess_pose.pitch = previous_pose.pitch;
    guess_pose.yaw = previous_pose.yaw + offset_yaw;

    Eigen::AngleAxisf init_rotation_x(guess_pose.roll, Eigen::Vector3f::UnitX());
    Eigen::AngleAxisf init_rotation_y(guess_pose.pitch, Eigen::Vector3f::UnitY());
    Eigen::AngleAxisf init_rotation_z(guess_pose.yaw, Eigen::Vector3f::UnitZ());
    Eigen::Translation3f init_translation(guess_pose.x, guess_pose.y, guess_pose.z);
    Eigen::Matrix4f init_guess = (init_translation * init_rotation_z * init_rotation_y * init_rotation_x).matrix() * tf_btol;

    pcl::PointCloud<pcl::PointXYZI>::Ptr output_cloud(new pcl::PointCloud<pcl::PointXYZI>);

    //位置合わせ  
    ndt.align(*output_cloud, init_guess);
    fitness_score = ndt.getFitnessScore();
    
    t_localizer = ndt.getFinalTransformation();
    t_base_link = t_localizer * tf_ltob;
    
    //output_cloudはボクセルによってフィルタ処理された点群，最終変換後のみの点群を得る
    //mapに追加する点群(transformed_scan_ptr)を得る
    pcl::transformPointCloud(*scan_ptr, *transformed_scan_ptr, t_localizer);
    
    //よくわからない
    tf::Matrix3x3 mat_l, mat_b;
    
    // mat_l.setValue(static_cast<double>(t_localizer(0, 0)), static_cast<double>(t_localizer(0, 1)), static_cast<double>(t_localizer(0, 2)),
    // 	  static_cast<double>(t_localizer(1, 0)), static_cast<double>(t_localizer(1, 1)), static_cast<double>(t_localizer(1, 2)),
    // 	  static_cast<double>(t_localizer(2, 0)), static_cast<double>(t_localizer(2, 1)), static_cast<double>(t_localizer(2, 2)));
    
    
    mat_b.setValue(static_cast<double>(t_base_link(0, 0)), static_cast<double>(t_base_link(0, 1)), static_cast<double>(t_base_link(0, 2)),
		   static_cast<double>(t_base_link(1, 0)), static_cast<double>(t_base_link(1, 1)), static_cast<double>(t_base_link(1, 2)),
		   static_cast<double>(t_base_link(2, 0)), static_cast<double>(t_base_link(2, 1)), static_cast<double>(t_base_link(2, 2)));


    // localizer_poseを更新
    // localizer_pose.x = t_localizer(0, 3);
    // localizer_pose.y = t_localizer(1, 3);
    // localizer_pose.z = t_localizer(2, 3);
    // mat_l.getRPY(localizer_pose.roll, localizer_pose.pitch, localizer_pose.yaw, 1);
    
    // ndt_poseを更新(自己位置に直接関係)
    ndt_pose.x = t_base_link(0, 3);
    ndt_pose.y = t_base_link(1, 3);
    ndt_pose.z = t_base_link(2, 3);
    mat_b.getRPY(ndt_pose.roll, ndt_pose.pitch, ndt_pose.yaw, 1);
    
    current_pose.x = ndt_pose.x;
    current_pose.y = ndt_pose.y;
    current_pose.z = ndt_pose.z;
    current_pose.roll = ndt_pose.roll;
    current_pose.pitch = ndt_pose.pitch;
    current_pose.yaw = ndt_pose.yaw;

    transform.setOrigin(tf::Vector3(current_pose.x, current_pose.y, current_pose.z));
    q.setRPY(current_pose.roll, current_pose.pitch, current_pose.yaw);
    transform.setRotation(q);
    
    br.sendTransform(tf::StampedTransform(transform, scan_time, "map", "base_link"));

    //offsetを計算 (curren_pose - previous_pose)
    offset_x = current_pose.x - previous_pose.x;
    offset_y = current_pose.y - previous_pose.y;
    offset_z = current_pose.z - previous_pose.z;
    offset_yaw = current_pose.yaw - previous_pose.yaw;


    //current_pos -> previous_pos
    previous_pose.x = current_pose.x;
    previous_pose.y = current_pose.y;
    previous_pose.z = current_pose.z;
    previous_pose.roll = current_pose.roll;
    previous_pose.pitch = current_pose.pitch;
    previous_pose.yaw = current_pose.yaw;    

    // map登録の点群数を減らすため
    pcl::PointCloud<pcl::PointXYZI>::Ptr filtered_transformed_scan_ptr(new pcl::PointCloud<pcl::PointXYZI>());
    voxel_grid_filter.setLeafSize(voxel_leaf_size1, voxel_leaf_size1, voxel_leaf_size1);
    voxel_grid_filter.setInputCloud(transformed_scan_ptr);
    voxel_grid_filter.filter(*filtered_transformed_scan_ptr);

    // added_poseとcurrent_pose間の移動距離を計算
    //mapの更新周期を移動距離で設定
    double shift = sqrt(pow(current_pose.x-added_pose.x, 2.0) + pow(current_pose.y-added_pose.y, 2.0));
    if(shift >= SHIFT){
      map += *filtered_transformed_scan_ptr;
      //map += *transformed_scan_ptr;
      added_pose.x = current_pose.x;
      added_pose.y = current_pose.y;
      added_pose.z = current_pose.z;
      added_pose.roll = current_pose.roll;
      added_pose.pitch = current_pose.pitch;
      added_pose.yaw = current_pose.yaw;
      isMapUpdate = true;
    }	

    //MAPをpublish
    sensor_msgs::PointCloud2::Ptr map_msg_ptr(new sensor_msgs::PointCloud2);
    pcl::toROSMsg(*map_ptr, *map_msg_ptr); //PointCloud<PointT>->PointCloud2
    // ndt_map_pub.publish(*map_msg_ptr);

    //現在の位置・姿勢をpublish
    // slam_msg.header.stamp = input->header.stamp;
    // slam_msg.x = current_pose.x;
    // slam_msg.y = current_pose.y;
    // slam_msg.z = current_pose.z;
    // slam_msg.yaw = current_pose.yaw;
    // slam_msg.roll = current_pose.roll;
    // slam_msg.pitch = current_pose.pitch;
    // slam_pub.publish(slam_msg);
    
    //現在位置をpublish
    current_point.point.x = current_pose.x;
    current_point.point.y = current_pose.y;
    current_point.point.z = current_pose.z;
    current_point.header.frame_id = "map";
    // current_point_pub.publish(current_point);

    //処理時間の計算
    ros::Time callback_end = ros::Time::now();
    ros::Duration d_callback = callback_end - callback_start;
   
    std::cout << "-----------------------------------------------------------------" << std::endl;
    std::cout << "Sequence number: " << input->header.seq << std::endl;
    std::cout << "Number of scan points: " << scan_ptr->size() << " points." << std::endl;
    std::cout << "Number of filtered scan points: " << filtered_scan_ptr->size() << " points." << std::endl;
    std::cout << "transformed_scan_ptr: " << transformed_scan_ptr->points.size() << " points." << std::endl;
    std::cout << "map: " << map.points.size() << " points." << std::endl;
    std::cout << "NDT has converged: " << ndt.hasConverged() << std::endl;
    std::cout << "Fitness score: " << fitness_score << std::endl;
    std::cout << "Number of iteration: " << ndt.getFinalNumIteration() << std::endl;
    std::cout << "(x,y,z,roll,pitch,yaw):" << std::endl;
    std::cout << "(" << current_pose.x << ", " << current_pose.y << ", " << current_pose.z << ", " << current_pose.roll << ", " << current_pose.pitch << ", " << current_pose.yaw << ")" << std::endl;
    std::cout << "Transformation Matrix:" << std::endl;
    std::cout << t_localizer << std::endl;
    std::cout << "shift: " << shift << std::endl;
    std::cout << "><><><><><><><><><><><><><><><><><><><><><><><><><><><><><><><" << std::endl;
    std::cout << "elapsed time: " << slam_time * 0.1 << "[s]" << std::endl; 
    // std::cout << "Published map Frame_id: " << map.header.frame_id << std::endl;
    std::cout << "time for 1 loop: " << d_callback << "[s]" << std::endl;
    std::cout << "><><><><><><><><><><><><><><><><><><><><><><><><><><><><><><><" << std::endl;
    std::cout << "-----------------------------------------------------------------" << std::endl;
    
    // ロボットのv,wの計算
    robot_v = (sqrt(offset_x*offset_x + offset_y*offset_y))*10;
    robot_v = 0.8*robot_pre_v + 0.2*robot_v;
    robot_w = offset_yaw;
    std::cout << "v: " << robot_v << std::endl;
    std::cout << "w: " << robot_w << std::endl;
    robot_pre_v = robot_v;
    robot_pub_v.data = static_cast<float>(robot_v);
    v_pub.publish(robot_pub_v);




    //処理時間データ,位置データ等をdatファイルに出力
   //  std::ofstream ofs("/home/rescue/data/slam/slam.dat", std::ios::out | std::ios::app);
// ofs << d_callback << " " << ndt.getFinalNumIteration() << " " << current_pose.x << " " << current_pose.y << " " << current_pose.z << " " << current_pose.roll << " " << current_pose.pitch << " " << current_pose.yaw << " " << robot_v << " " << robot_w << " " << slam_time*0.1 << std::endl;
//     ofs.close();

    slam_data_save();

    slam_time++;

 }

   


int main(int argc, char **argv)
{
    previous_pose.x = 0.0;
    previous_pose.y = 0.0;
    previous_pose.z = 0.0;
    previous_pose.roll = 0.0;
    previous_pose.pitch = 0.0;
    previous_pose.yaw = 0.0;

    ndt_pose.x = 0.0;
    ndt_pose.y = 0.0;
    ndt_pose.z = 0.0;
    ndt_pose.roll = 0.0;
    ndt_pose.pitch = 0.0;
    ndt_pose.yaw = 0.0;

    current_pose.x = 0.0;
    current_pose.y = 0.0;
    current_pose.z = 0.0;
    current_pose.roll = 0.0;
    current_pose.pitch = 0.0;
    current_pose.yaw = 0.0;

    guess_pose.x = 0.0;
    guess_pose.y = 0.0;
    guess_pose.z = 0.0;
    guess_pose.roll = 0.0;
    guess_pose.pitch = 0.0;
    guess_pose.yaw = 0.0;

    added_pose.x = 0.0;
    added_pose.y = 0.0;
    added_pose.z = 0.0;
    added_pose.roll = 0.0;
    added_pose.pitch = 0.0;
    added_pose.yaw = 0.0;

    offset_x = 0.0;
    offset_y = 0.0;
    offset_z = 0.0;
    offset_yaw = 0.0;
   
    ros::init(argc, argv, "ndt_localize");

    ros::NodeHandle nh;
    ros::NodeHandle private_nh("~");

    // launchファイルからパラメータの取得
    private_nh.getParam("range", RANGE);
    std::cout << "RANGE: " << RANGE << std::endl;
    private_nh.getParam("shift", SHIFT);
    std::cout << "SHIFT: " << SHIFT << std::endl;
    private_nh.getParam("use_openmp", _use_openmp);
    std::cout << "use_openmp: " << _use_openmp << std::endl;

    std::cout << "(tf_x,tf_y,tf_z,tf_roll,tf_pitch,tf_yaw): (" << _tf_x << ", " << _tf_y << ", " << _tf_z << ", "
              << _tf_roll << ", " << _tf_pitch << ", " << _tf_yaw << ")" << std::endl;


    Eigen::Translation3f tl_btol(_tf_x, _tf_y, _tf_z);  // tl: translation
    Eigen::AngleAxisf rot_x_btol(_tf_roll, Eigen::Vector3f::UnitX());  // rot: rotation
    Eigen::AngleAxisf rot_y_btol(_tf_pitch, Eigen::Vector3f::UnitY());
    Eigen::AngleAxisf rot_z_btol(_tf_yaw, Eigen::Vector3f::UnitZ());
    tf_btol = (tl_btol * rot_z_btol * rot_y_btol * rot_x_btol).matrix();

    Eigen::Translation3f tl_ltob((-1.0) * _tf_x, (-1.0) * _tf_y, (-1.0) * _tf_z);  // tl: translation
    Eigen::AngleAxisf rot_x_ltob((-1.0) * _tf_roll, Eigen::Vector3f::UnitX());  // rot: rotation
    Eigen::AngleAxisf rot_y_ltob((-1.0) * _tf_pitch, Eigen::Vector3f::UnitY());
    Eigen::AngleAxisf rot_z_ltob((-1.0) * _tf_yaw, Eigen::Vector3f::UnitZ());
    tf_ltob = (tl_ltob * rot_z_ltob * rot_y_ltob * rot_x_ltob).matrix();

    map.header.frame_id = "map";

     
    remove("/home/rescue-nuc/data/slam/slam.dat");

    //publishers
    // ndt_map_pub = nh.advertise<sensor_msgs::PointCloud2>("/rescue_robot/lidar/ndt_map", 1000);//あるとクソ重くなる可視化用
    // current_point_pub = nh.advertise<geometry_msgs::PointStamped>("/rescue_robot/lidar/current_point", 1);//多分自己位置
    // slam_pub = nh.advertise<project_msgs::Slam>("/rescue_robot/lidar/slam_result", 1);//の情報多い晩
    v_pub = nh.advertise<std_msgs::Float32>("/robot_v", 1);

    //Subscribers
    ros::Subscriber points_sub = nh.subscribe("/unilidar/cloud", 100000, points_callback);

    ros::spin();

    return 0;
} 
