#include "pfxobjects.h"

#include <cstring>
#include <cassert>

#include "light.h"
#include "particlefx.h"
#include "pose.h"
#include "rendererstorage.h"
#include "skeleton.h"

using namespace Tempest;

std::mt19937 PfxObjects::rndEngine;

PfxObjects::Emitter::Emitter(PfxObjects::Bucket& b, size_t id)
  :bucket(&b), id(id) {
  }

PfxObjects::Emitter::~Emitter() {
  if(bucket) {
    auto& p  = bucket->impl[id];
    p.alive  = false;
    p.active = false;
    }
  }

PfxObjects::Emitter::Emitter(PfxObjects::Emitter && b)
  :bucket(b.bucket), id(b.id) {
  b.bucket = nullptr;
  }

PfxObjects::Emitter &PfxObjects::Emitter::operator=(PfxObjects::Emitter &&b) {
  std::swap(bucket,b.bucket);
  std::swap(id,b.id);
  return *this;
  }

void PfxObjects::Emitter::setPosition(float x, float y, float z) {
  if(bucket==nullptr)
    return;
  auto& v = bucket->impl[id];
  v.pos = Vec3(x,y,z);
  if(bucket->impl[id].block==size_t(-1))
    return; // no backup memory
  auto& p = bucket->getBlock(*this);
  p.pos = Vec3(x,y,z);
  }

void PfxObjects::Emitter::setActive(bool act) {
  if(bucket==nullptr)
    return;
  bucket->impl[id].active = act;
  }

void PfxObjects::Emitter::setObjMatrix(const Matrix4x4 &mt) { //fixme: usless for Npc
  setPosition(mt.at(3,0),mt.at(3,1),mt.at(3,2));
  }

PfxObjects::Bucket::Bucket(const RendererStorage &storage, const ParticleFx &ow, PfxObjects *parent)
  :owner(&ow), parent(parent) {
  auto cnt = storage.device.maxFramesInFlight();

  pf.reset(new PerFrame[cnt]);
  for(size_t i=0;i<cnt;++i)
    pf[i].ubo = storage.device.uniforms(storage.uboPfxLayout());

  uint64_t lt      = ow.maxLifetime();
  uint64_t pps     = uint64_t(std::ceil(ow.ppsValue));
  uint64_t reserve = (lt*pps+1000-1)/1000;
  blockSize        = size_t(reserve);
  }

size_t PfxObjects::Bucket::allocBlock() {
  for(size_t i=0;i<block.size();++i) {
    if(!block[i].alive) {
      block[i].alive=true;
      return i;
      }
    }

  block.emplace_back();

  Block& b = block.back();
  b.offset = particles.size();

  particles.resize(particles.size()+blockSize);
  vbo.resize(particles.size()*6);
  parent->invalidateCmd();

  for(size_t i=0; i<blockSize; ++i)
    particles[i].life = 0;
  return block.size()-1;
  }

void PfxObjects::Bucket::freeBlock(size_t& i) {
  if(i==size_t(-1))
    return;
  auto& b = block[i];
  assert(b.count==0);
  b.alive = false;
  b.alive = false;
  i = size_t(-1);
  }

PfxObjects::Block &PfxObjects::Bucket::getBlock(PfxObjects::ImplEmitter &e) {
  if(e.block==size_t(-1)) {
    e.block = allocBlock();
    auto& p = block[e.block];
    p.pos   = e.pos;
    }
  return block[e.block];
  }

PfxObjects::Block &PfxObjects::Bucket::getBlock(PfxObjects::Emitter &e) {
  return getBlock(impl[e.id]);
  }

size_t PfxObjects::Bucket::allocEmitter() {
  for(size_t i=0; i<impl.size(); ++i) {
    auto& b = impl[i];
    if(!b.alive && b.block==size_t(-1)) {
      b.alive = true;
      return i;
      }
    }
  impl.emplace_back();
  auto& e = impl.back();
  e.block = size_t(-1); // no backup memory

  return impl.size()-1;
  }

bool PfxObjects::Bucket::shrink() {
  while(impl.size()>0) {
    auto& b = impl.back();
    if(b.alive || b.block!=size_t(-1))
      break;
    impl.pop_back();
    }
  while(block.size()>0) {
    auto& b = block.back();
    if(b.alive || b.count>0)
      break;
    block.pop_back();
    }
  if(particles.size()!=block.size()*blockSize) {
    particles.resize(block.size()*blockSize);
    vbo.resize(particles.size()*6);
    parent->invalidateCmd();
    return true;
    }
  return false;
  }

void PfxObjects::Bucket::init(size_t particle) {
  auto& p = particles[particle];

  float dx=0,dy=0,dz=0;

  if(owner->shpType_S==ParticleFx::EmitterType::Point) {
    dx = 0;
    dy = 0;
    dz = 0;
    }
  else if(owner->shpType_S==ParticleFx::EmitterType::Sphere) {
    float theta = float(2.0*M_PI)*randf();
    float phi   = std::acos(1.f - 2.f * randf());
    dx    = std::sin(phi) * std::cos(theta);
    dy    = std::sin(phi) * std::sin(theta);
    dz    = std::cos(phi);
    }
  else if(owner->shpType_S==ParticleFx::EmitterType::Box) {
    dx  = randf()*2.f-1.f;
    dy  = randf()*2.f-1.f;
    dz  = randf()*2.f-1.f;
    } else {
    p.pos = Vec3();
    }
  Vec3 dim = owner->shpDim_S*0.5f;
  p.pos = Vec3(dx*dim.x,dy*dim.y,dz*dim.z)+owner->shpOffsetVec_S;

  if(owner->dirMode_S==ParticleFx::Dir::Rand) {
    p.rotation  = randf()*float(2.0*M_PI);
    }
  else if(owner->dirMode_S==ParticleFx::Dir::Dir) {
    p.rotation  = (owner->dirAngleHead+(2.f*randf()-1.f)*owner->dirAngleHeadVar)*float(M_PI)/180.f;
    p.drotation = (owner->dirAngleElev+(2.f*randf()-1.f)*owner->dirAngleElevVar)*float(M_PI)/180.f;
    }
  else if(owner->dirMode_S==ParticleFx::Dir::Target) {
    // p.rotation  = std::atan2(p.pos.y,p.pos.x);
    p.rotation  = randf()*float(2.0*M_PI); //FIXME
    }

  p.life    = uint16_t(owner->lspPartAvg+owner->lspPartVar*(2.f*randf()-1.f));
  p.maxLife = p.life;
  }

void PfxObjects::Bucket::finalize(size_t particle) {
  Vertex* v = &vbo[particle*6];
  std::memset(v,0,sizeof(*v)*6);
  }

float PfxObjects::ParState::lifeTime() const {
  return 1.f-life/float(maxLife);
  }

PfxObjects::PfxObjects(const RendererStorage& storage)
  :storage(storage),uboGlobalPf(storage.device) {
  updateCmd.resize(storage.device.maxFramesInFlight());
  }

PfxObjects::Emitter PfxObjects::get(const ParticleFx &decl) {
  auto&  b = getBucket(decl);
  size_t e = b.allocEmitter();
  return Emitter(b,e);
  }

void PfxObjects::setModelView(const Tempest::Matrix4x4 &m,const Tempest::Matrix4x4 &shadow) {
  uboGlobal.modelView  = m;
  uboGlobal.shadowView = shadow;
  }

void PfxObjects::setLight(const Light &l, const Vec3 &ambient) {
  auto  d = l.dir();
  auto& c = l.color();

  uboGlobal.lightDir = {-d[0],-d[1],-d[2]};
  uboGlobal.lightCl  = {c.x,c.y,c.z,0.f};
  uboGlobal.lightAmb = {ambient.x,ambient.y,ambient.z,0.f};
  }

void PfxObjects::setViewerPos(const Vec3& pos) {
  viewePos = pos;
  }

bool PfxObjects::needToUpdateCommands(uint8_t fId) const {
  return updateCmd[fId];
  }

void PfxObjects::setAsUpdated(uint8_t fId) {
  updateCmd[fId]=false;
  }

void PfxObjects::resetTicks() {
  lastUpdate = size_t(-1);
  }

void PfxObjects::tick(uint64_t ticks) {
  if(lastUpdate>ticks) {
    lastUpdate = ticks;
    return;
    }

  uint64_t dt    = ticks-lastUpdate;
  float    dtFlt = float(dt)/1000.f;

  if(dt==0)
    return;

  for(auto& i:bucket) {
    tickSys(i,dt,dtFlt);
    buildVbo(i);
    }
  lastUpdate = ticks;
  }

void PfxObjects::updateUbo(uint8_t frameId) {
  uboGlobalPf.update(uboGlobal,frameId);
  for(auto& i:bucket) {
    auto& pf = i.pf[frameId];
    pf.vbo.update(i.vbo);
    }
  }

void PfxObjects::commitUbo(uint8_t frameId, const Texture2d& shadowMap) {
  size_t pfCount = storage.device.maxFramesInFlight();
  bucket.remove_if([pfCount](const Bucket& b) {
    for(size_t i=0;i<pfCount;++i) {
      if(b.pf[i].vbo.size()>0)
        return false;
      }
    return b.impl.size()==0;
    });

  if(!updateCmd[frameId])
    return;

  for(auto& i:bucket) {
    auto& pf = i.pf[frameId];
    if(i.vbo.size()!=pf.vbo.size())
      pf.vbo = storage.device.vboDyn(i.vbo);
    pf.ubo.set(0,uboGlobalPf[frameId],0,1);
    pf.ubo.set(2,*i.owner->visName_S);
    pf.ubo.set(3,shadowMap);
    }
  }

void PfxObjects::draw(Tempest::Encoder<Tempest::CommandBuffer> &cmd, uint32_t imgId) {
  for(auto& i:bucket) {
    auto& pf = i.pf[imgId];
    uint32_t offset=0;
    cmd.setUniforms(storage.pPfx,pf.ubo,1,&offset);
    cmd.draw(pf.vbo);
    }
  }

float PfxObjects::randf() {
  return float(rndEngine()%10000)/10000.f;
  }

PfxObjects::Bucket &PfxObjects::getBucket(const ParticleFx &ow) {
  for(auto& i:bucket)
    if(i.owner==&ow)
      return i;
  bucket.push_front(Bucket(storage,ow,this));
  return *bucket.begin();
  }

void PfxObjects::tickSys(PfxObjects::Bucket &b, uint64_t dtMilis, float dt) {
  bool doShrink = false;
  for(auto& emitter:b.impl) {
    const auto dp      = emitter.pos-viewePos;
    const bool active  = emitter.active;
    const bool nearby  = (dp.quadLength()<4000*4000);
    const bool process = active && nearby;

    if(!process && emitter.block==size_t(-1)) {
      continue;
      }

    auto& p = b.getBlock(emitter);
    if(p.count>0) {
      tickSys(b,p,dtMilis,dt);
      if(p.count==0 && !process){
        // free mem
        b.freeBlock(emitter.block);
        doShrink = true;
        continue;
        }
      }

    p.timeTotal+=dtMilis;
    uint64_t fltScale = 100;
    uint64_t emited   = uint64_t(p.timeTotal*uint64_t(b.owner->ppsValue*float(fltScale)))/uint64_t(1000u*fltScale);

    if(!nearby) {
      p.emited = emited;
      }
    else if(active) {
      tickSysEmit(b,p,emited);
      }
    }

  if(doShrink)
    b.shrink();
  }

void PfxObjects::tickSys(Bucket& b, Block& p, uint64_t dtMilis, float dt) {
  for(size_t i=0;i<b.blockSize;++i) {
    ParState& ps = b.particles[i+p.offset];
    if(ps.life==0)
      continue;
    if(ps.life<=dtMilis){
      ps.life = 0;
      p.count--;
      b.finalize(i+p.offset);
      } else {
      // eval particle
      ps.life  = uint16_t(ps.life-dtMilis);
      ps.pos  += ps.dir*dt;
      ps.pos  += b.owner->flyGravity_S;
      }
    }
  }

void PfxObjects::tickSysEmit(PfxObjects::Bucket& b, PfxObjects::Block& p, uint64_t emited) {
  while(p.emited<emited) {
    p.emited++;

    for(size_t i=0;i<b.blockSize;++i) {
      ParState& ps = b.particles[i+p.offset];
      if(ps.life==0) { // free slot
        p.count++;
        b.init(i+p.offset);
        break;
        }
      }
    }
  }

static void rotate(float* rx,float* ry,float a,const float* x, const float* y){
  const float c = std::cos(a);
  const float s = std::sin(a);

  for(int i=0;i<3;++i) {
    rx[i] = x[i]*c - y[i]*s;
    ry[i] = x[i]*s + y[i]*c;
    }
  }

void PfxObjects::buildVbo(PfxObjects::Bucket &b) {
  static const float dx[6] = {-0.5f, 0.5f, -0.5f,  0.5f,  0.5f, -0.5f};
  static const float dy[6] = { 0.5f, 0.5f, -0.5f,  0.5f, -0.5f, -0.5f};

  const auto& m = uboGlobal.modelView;
  float left[4] = {m.at(0,0),m.at(1,0),m.at(2,0)};
  float top [4] = {m.at(0,1),m.at(1,1),m.at(2,1)};

  left[3] = std::sqrt(left[0]*left[0]+left[1]*left[1]+left[2]*left[2]);
  top [3] = std::sqrt( top[0]* top[0]+ top[1]* top[1]+ top[2]* top[2]);

  for(int i=0;i<3;++i) {
    left[i] /= left[3];
    top [i] /= top [3];
    }

  auto& ow              = *b.owner;
  auto  colorS          = ow.visTexColorStart_S;
  auto  colorE          = ow.visTexColorEnd_S;
  auto  visSizeStart_S  = ow.visSizeStart_S;
  auto  visSizeEndScale = ow.visSizeEndScale;
  auto  visAlphaStart   = ow.visAlphaStart;
  auto  visAlphaEnd     = ow.visAlphaEnd;
  auto  visAlphaFunc    = ow.visAlphaFunc_S;

  for(auto& p:b.block) {
    if(p.count==0)
      continue;

    for(size_t i=0;i<b.blockSize;++i) {
      ParState& ps = b.particles[i+p.offset];
      Vertex*   v  = &b.vbo[(p.offset+i)*6];

      const float a   = ps.lifeTime();
      const Vec3  cl  = colorS*(1.f-a)        + colorE*a;
      const float clA = visAlphaStart*(1.f-a) + visAlphaEnd*a;

      const float szX = visSizeStart_S.x*(1.f + a*(visSizeEndScale-1.f));
      const float szY = visSizeStart_S.y*(1.f + a*(visSizeEndScale-1.f));

      float l[3]={};
      float t[3]={};
      rotate(l,t,ps.rotation,left,top);

      struct Color {
        uint8_t r=0;
        uint8_t g=0;
        uint8_t b=0;
        uint8_t a=0;
        } color;

      if(visAlphaFunc==ParticleFx::AlphaFunc::Add) {
        color.r = uint8_t(cl.x*clA);
        color.g = uint8_t(cl.y*clA);
        color.b = uint8_t(cl.z*clA);
        color.a = uint8_t(255);
        }
      else if(visAlphaFunc==ParticleFx::AlphaFunc::Blend) {
        color.r = uint8_t(cl.x);
        color.g = uint8_t(cl.y);
        color.b = uint8_t(cl.z);
        color.a = uint8_t(clA*255);
        }

      for(int i=0;i<6;++i) {
        float sx = l[0]*dx[i]*szX + t[0]*dy[i]*szY;
        float sy = l[1]*dx[i]*szX + t[1]*dy[i]*szY;
        float sz = l[2]*dx[i]*szX + t[2]*dy[i]*szY;

        v[i].pos[0] = ps.pos.x + p.pos.x + sx;
        v[i].pos[1] = ps.pos.y + p.pos.y + sy;
        v[i].pos[2] = ps.pos.z + p.pos.z + sz;

        v[i].uv[0]  = (dx[i]+0.5f);//float(ow.frameCount);
        v[i].uv[1]  = (dy[i]+0.5f);

        std::memcpy(&v[i].color,&color,4);
        }
      }
    }
  }

void PfxObjects::invalidateCmd() {
  for(size_t i=0;i<updateCmd.size();++i)
    updateCmd[i]=true;
  }
