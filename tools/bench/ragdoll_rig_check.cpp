#include "native_ragdoll_rig.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <random>
#include <string_view>

namespace {
using entity_transform::Transform;
using entity_transform::Vec3;
using native_ragdoll::SkeletonPose;
size_t checks = 0;

void Check(bool condition, std::string_view message) {
  ++checks;
  if (!condition) {
    std::cerr << "FAIL: " << message << '\n';
    std::exit(1);
  }
}

Transform Identity() { return {{1,0,0},{0,1,0},{0,0,1},{0,0,0}}; }

// Independent homogeneous matrix multiplication oracle. Storage follows the
// row-vector convention documented by the original RenderWare routines.
Transform Multiply(const Transform& parent, const Transform& local) {
  const double a[4][4] = {{local.right[0],local.right[1],local.right[2],0},
      {local.up[0],local.up[1],local.up[2],0},
      {local.forward[0],local.forward[1],local.forward[2],0},
      {local.position[0],local.position[1],local.position[2],1}};
  const double b[4][4] = {{parent.right[0],parent.right[1],parent.right[2],0},
      {parent.up[0],parent.up[1],parent.up[2],0},
      {parent.forward[0],parent.forward[1],parent.forward[2],0},
      {parent.position[0],parent.position[1],parent.position[2],1}};
  double c[4][4]{};
  for (size_t row=0;row<4;++row)
    for (size_t column=0;column<4;++column)
      for (size_t k=0;k<4;++k) c[row][column]+=a[row][k]*b[k][column];
  return {{float(c[0][0]),float(c[0][1]),float(c[0][2])},
      {float(c[1][0]),float(c[1][1]),float(c[1][2])},
      {float(c[2][0]),float(c[2][1]),float(c[2][2])},
      {float(c[3][0]),float(c[3][1]),float(c[3][2])}};
}

bool Near(const Transform& a, const Transform& b, float tolerance=2e-4f) {
  const Vec3 av[]={a.right,a.up,a.forward,a.position};
  const Vec3 bv[]={b.right,b.up,b.forward,b.position};
  for (size_t i=0;i<4;++i)
    for (size_t k=0;k<3;++k)
      if (!std::isfinite(av[i][k]) || std::abs(av[i][k]-bv[i][k])>tolerance) return false;
  return true;
}

Transform Rotation(float angle, size_t axis) {
  auto value=Identity();
  const float c=std::cos(angle),s=std::sin(angle);
  if(axis==0) {value.up={0,c,s};value.forward={0,-s,c};}
  if(axis==1) {value.right={c,0,-s};value.forward={s,0,c};}
  if(axis==2) {value.right={c,s,0};value.up={-s,c,0};}
  return value;
}

Transform QuaternionFrame(const native_ragdoll::CapsuleBodyInfo& body) {
  const double x=body.rotation[0],y=body.rotation[1],z=body.rotation[2],w=body.rotation[3];
  return {{float(1-2*(y*y+z*z)),float(2*(x*y+z*w)),float(2*(x*z-y*w))},
      {float(2*(x*y-z*w)),float(1-2*(x*x+z*z)),float(2*(y*z+x*w))},
      {float(2*(x*z+y*w)),float(2*(y*z-x*w)),float(1-2*(x*x+y*y))},body.position};
}

uint32_t BoneID(std::string_view text) {
  uint32_t hash=0;
  for(const unsigned char letter:text) hash=hash*65599u+letter;
  return hash;
}

SkeletonPose Skeleton(float size=1) {
  SkeletonPose pose;
  pose.actor=1;pose.world_epoch=7;pose.model_guid={1,2,3,4};pose.world_transform=Identity();
  const auto add=[&](std::string_view name,int32_t parent,Vec3 position) {
    auto transform=Identity();
    for(auto& value:position)value*=size;
    transform.position=position;
    pose.bones.push_back({BoneID(name),uint32_t(pose.bones.size()),parent,transform});
  };
  add("root",-1,{0,0,0});
  add("m_pelvis",0,{0,.9f,0});
  add("m_thoracic",1,{0,1.2f,0});
  add("m_neck1",2,{0,1.5f,0});
  add("m_neck2",3,{0,1.55f,0});
  add("l_eye",4,{-.07f,1.7f,.04f});
  add("r_eye",4,{.07f,1.7f,.04f});
  add("m_jaw",4,{0,1.6f,.05f});
  add("l_hip",1,{-.12f,.9f,0});
  add("l_knee",8,{-.12f,.5f,.02f});
  add("l_ankle",9,{-.12f,.12f,0});
  add("l_ball",10,{-.12f,.06f,.15f});
  add("r_hip",1,{.12f,.9f,0});
  add("r_knee",12,{.12f,.5f,.02f});
  add("r_ankle",13,{.12f,.12f,0});
  add("r_ball",14,{.12f,.06f,.15f});
  add("l_clav",2,{-.1f,1.4f,0});
  add("l_shoulder",16,{-.2f,1.4f,0});
  add("l_elbow",17,{-.45f,1.3f,0});
  add("l_wrist",18,{-.7f,1.2f,.03f});
  add("l_finger",19,{-.72f,1.2f,.04f});
  add("r_clav",2,{.1f,1.4f,0});
  add("r_shoulder",21,{.2f,1.4f,0});
  add("r_elbow",22,{.45f,1.3f,0});
  add("r_wrist",23,{.7f,1.2f,.03f});
  add("cape",2,{0,1.1f,-.05f});
  pose.model_nodes=uint32_t(pose.bones.size()+3);
  return pose;
}

void CheckRig(const SkeletonPose& pose) {
  const auto rig=native_ragdoll::BuildBipedRig(pose,77,{1,2,3});
  Check(bool(rig),"complete named biped accepted");
  Check(rig->bodies.size()==13 && rig->joints.size()==12,"13 bodies and connected 12-joint tree");
  std::vector<native_ragdoll::SimulatedBody> bodies;
  double mass=0;
  for(const auto& body:rig->bodies) {
    mass+=body.mass;
    Check(body.collision_filter==0,"collision filter stays owned by native publication");
    Check(body.linear_velocity==Vec3{1,2,3},"inherited world velocity");
    Check(body.radius>0 && body.half_length>0,"positive capsule dimensions");
    double norm=0;for(auto v:body.rotation)norm+=double(v)*v;
    Check(std::abs(norm-1)<1e-6,"normalized body quaternion");
    bodies.push_back({QuaternionFrame(body),{}, {}});
  }
  Check(std::abs(mass-77)<1e-4,"mass fractions sum to requested total");
  for(const auto& joint:rig->joints)
    Check(Near(Multiply(bodies[joint.body_a].transform,joint.frame_a),
        Multiply(bodies[joint.body_b].transform,joint.frame_b)),"both native joint frames coincide in world space");
  constexpr float radians=3.14159265358979323846f/180;
  for(size_t i=0;i<rig->joints.size();++i) {
    const auto& profile=rig->joints[i].limits;
    Check(bool(profile),"every biped joint carries its explicit passive limit profile");
    Check(profile->twist_min<=0 && profile->twist_max>=0 &&
          profile->plane_min<=0 && profile->plane_max>=0 && profile->cone_max>0,
          "the captured entry pose lies within every passive limit");
    if(i==3 || i==6 || i==9 || i==11)
      Check(std::abs(profile->twist_max-5*radians)<1e-6f &&
            std::abs(profile->plane_max-5*radians)<1e-6f && profile->cone_max>2,
            "knees/elbows allow bending while bounding lateral/twist motion");
  }
  const auto result=native_ragdoll::EvaluateBipedRig(*rig,bodies,pose.world_transform);
  Check(bool(result) && result->size()==pose.bones.size(),"complete reconstructed animation palette");
  for(size_t i=0;i<result->size();++i) {
    Check(Near((*result)[i].model_transform,pose.bones[i].model_transform),"initial skeleton reconstructed with scale and helpers");
    Check((*result)[i].id==pose.bones[i].id && (*result)[i].model_index==pose.bones[i].model_index &&
        (*result)[i].parent==pose.bones[i].parent,"native track identity retained");
  }
  auto movement=Rotation(1.4f,0);movement.position={.6f,.7f,-.3f};
  for(auto& body:bodies)body.transform=Multiply(movement,body.transform);
  auto root=Multiply(Rotation(-.3f,2),pose.world_transform);root.position={-2,3,4};
  const auto moved=native_ragdoll::EvaluateBipedRig(*rig,bodies,root);
  Check(bool(moved),"physics-to-model conversion accepts independently moved render root");
  for(size_t i=0;i<moved->size();++i)
    Check(Near(Multiply(root,(*moved)[i].model_transform),
        Multiply(movement,Multiply(pose.world_transform,pose.bones[i].model_transform)),5e-4f),
        "world-space physics motion preserves every authored affine bone");
  bodies[4].transform.right=bodies[4].transform.up;
  Check(!native_ragdoll::EvaluateBipedRig(*rig,bodies,root),"nonrigid physics readback rejected");
}
}  // namespace

int main() {
  std::mt19937 random(11909);
  std::uniform_real_distribution<float> angle(-3.14f,3.14f),scale(.3f,2.5f),shift(-10,10);
  for(size_t trial=0;trial<1000;++trial) {
    auto parent=Multiply(Rotation(angle(random),trial%3),Rotation(angle(random),(trial+1)%3));
    for(auto* v:{&parent.right,&parent.up,&parent.forward})for(auto& axis:*v)axis*=scale(random);
    parent.position={shift(random),shift(random),shift(random)};
    if(!entity_transform::Valid(parent))continue;
    auto local=Rotation(angle(random),trial%3);local.position={shift(random),shift(random),shift(random)};
    const auto world=entity_transform::Compose(parent,local);
    Check(world && Near(*world,Multiply(parent,local)),"affine composition matches independent matrix oracle");
    const auto restored=entity_transform::Relative(parent,*world);
    Check(restored && Near(*restored,local,.004f),"sheared affine inverse round trip");
  }
  auto bad=Identity();bad.forward=bad.right;
  Check(!entity_transform::Compose(bad,Identity()) && !entity_transform::Relative(bad,Identity()),"singular transforms rejected");
  for(size_t trial=0;trial<120;++trial) {
    auto pose=Skeleton(.5f+.01f*float(trial));
    pose.world_transform=Multiply(Rotation(angle(random),trial%3),Rotation(angle(random),(trial+1)%3));
    pose.world_transform.position={shift(random),shift(random),shift(random)};
    // Authored nonuniform scale/shear belongs to skinning, never rigid bodies.
    for(auto& bone:pose.bones) {
      bone.model_transform.right[0]=1.2f;
      bone.model_transform.up={.12f,.9f,0};
      bone.model_transform.forward[2]=1.1f;
    }
    CheckRig(pose);
  }
  auto pose=Skeleton();pose.bones[18].model_transform.position={-.2f,1.4f,.3f};
  CheckRig(pose); // upper arm parallel to facing: use a stable fallback axis
  pose=Skeleton();pose.bones[3].id=123;
  Check(!native_ragdoll::BuildBipedRig(pose,70),"missing anatomical bone rejected");
  pose=Skeleton();pose.bones[9].parent=9;
  Check(!native_ragdoll::BuildBipedRig(pose,70),"cyclic parent rejected");
  pose=Skeleton();pose.bones[9].parent=1;
  Check(!native_ragdoll::BuildBipedRig(pose,70),"anatomically unrelated knee rejected");
  pose=Skeleton();pose.bones[9].model_index=pose.bones[8].model_index;
  Check(!native_ragdoll::BuildBipedRig(pose,70),"duplicate palette slot rejected");
  pose=Skeleton();pose.bones[9].model_transform.position=pose.bones[8].model_transform.position;
  Check(!native_ragdoll::BuildBipedRig(pose,70),"zero-length limb rejected");
  pose=Skeleton();pose.world_transform.right[0]=std::numeric_limits<float>::infinity();
  Check(!native_ragdoll::BuildBipedRig(pose,70),"nonfinite model root rejected");
  pose=Skeleton();
  Check(!native_ragdoll::BuildBipedRig(pose,-1),"negative mass rejected");
  Check(!native_ragdoll::BuildBipedRig(pose,70,{0,std::numeric_limits<float>::quiet_NaN(),0}),"nonfinite velocity rejected");
  pose.bones.back().model_transform.right={0,0,0};
  pose.bones.back().model_transform.up={0,0,0};
  pose.bones.back().model_transform.forward={0,0,0};
  CheckRig(pose); // authored hidden geometry must stay collapsed
  std::cout << "PASS: " << checks << " anatomical rig and affine pose checks\n";
}
