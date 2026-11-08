// Independent CPU area/bilinear oracle for both eyes' private colour raster.
namespace quality {
void color_scale_checks(ID3D12Device* device,ID3D12CommandQueue* queue) {
    constexpr UINT w=37,h=29;
    D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width=w;rd.Height=h;rd.DepthOrArraySize=rd.MipLevels=1;rd.SampleDesc.Count=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    ComPtr<ID3D12Resource> source;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&source)),"scale source");
    const auto buffer=[&](UINT64 size,D3D12_HEAP_TYPE type) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=size;d.Height=1;d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> out;check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,type==D3D12_HEAP_TYPE_UPLOAD?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out)),"scale buffer");return out;
    };
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"scale allocator");
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"scale list");
    ComPtr<ID3D12Fence> fence;check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"scale fence");HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);UINT64 value{};
    const auto submit=[&]{check(list->Close(),"scale close");ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);check(queue->Signal(fence.Get(),++value),"scale signal");check(fence->SetEventOnCompletion(value,event),"scale event");require(WaitForSingleObject(event,10000)==WAIT_OBJECT_0,"scale timeout");};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};UINT64 size{};device->GetCopyableFootprints(&rd,0,1,0,&fp,nullptr,nullptr,&size);
    auto upload=buffer(size,D3D12_HEAP_TYPE_UPLOAD),read=buffer(size,D3D12_HEAP_TYPE_READBACK);void* data{};check(upload->Map(0,nullptr,&data),"scale map");
    std::vector<unsigned char> original(w*h*4);
    for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)for(UINT c=0;c<4;++c)original[(y*w+x)*4+c]=static_cast<unsigned char>((x*37+y*71+c*53)%256);
    for(UINT y=0;y<h;++y)std::memcpy(static_cast<char*>(data)+fp.Offset+y*fp.Footprint.RowPitch,original.data()+y*w*4,w*4);upload->Unmap(0,nullptr);
    auto b=transition(source.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);list->ResourceBarrier(1,&b);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=fp;to.pResource=source.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);submit();
    for(UINT factor:{1u,2u,4u}) {
        const UINT lw=factor==1 ? w-2 : (w+factor-1)/factor;
        const UINT lh=factor==1 ? h-1 : (h+factor-1)/factor;
        rd.Width=lw;rd.Height=lh;rd.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ComPtr<ID3D12Resource> reduced_texture;check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&reduced_texture)),"scale reduced_texture");
        xrfg::AfwColorScale scale;check(scale.prepare(device,w,h,rd.Format),"scale pipeline");check(allocator->Reset(),"scale reset");check(list->Reset(allocator.Get(),nullptr),"scale reset list");
        scale.record(device,list.Get(),source.Get(),D3D12_RESOURCE_STATE_COMMON,w,h,reduced_texture.Get(),D3D12_RESOURCE_STATE_COMMON,lw,lh,0);
        scale.record(device,list.Get(),reduced_texture.Get(),D3D12_RESOURCE_STATE_COMMON,lw,lh,scale.expanded(),D3D12_RESOURCE_STATE_COMMON,w,h,1);
        b=transition(scale.expanded(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);list->ResourceBarrier(1,&b);from.pResource=scale.expanded();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.pResource=read.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=fp;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);std::swap(b.Transition.StateBefore,b.Transition.StateAfter);list->ResourceBarrier(1,&b);submit();
        std::vector<double> reduced(lw*lh*4);
        for(UINT y=0;y<lh;++y)for(UINT x=0;x<lw;++x)for(UINT c=0;c<4;++c) {
            const double x0=double(x)*w/lw,x1=double(x+1)*w/lw,y0=double(y)*h/lh,y1=double(y+1)*h/lh;double sum{};
            for(int sy=int(std::floor(y0));sy<int(std::ceil(y1));++sy)for(int sx=int(std::floor(x0));sx<int(std::ceil(x1));++sx)
                sum+=original[(sy*w+sx)*4+c]*(std::min(x1,double(sx+1))-std::max(x0,double(sx)))*(std::min(y1,double(sy+1))-std::max(y0,double(sy)));
            reduced[(y*lw+x)*4+c]=std::round(sum/((x1-x0)*(y1-y0)));
        }
        check(read->Map(0,nullptr,&data),"scale read");double max_error{};
        for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)for(UINT c=0;c<4;++c) {
            const double px=(x+.5)*lw/w-.5,py=(y+.5)*lh/h-.5;const int ix=int(std::floor(px)),iy=int(std::floor(py));double expected{};
            for(int dy=0;dy<2;++dy)for(int dx=0;dx<2;++dx) {
                const auto sx=std::clamp(ix+dx,0,int(lw)-1),sy=std::clamp(iy+dy,0,int(lh)-1);
                expected+=reduced[(sy*lw+sx)*4+c]*(dx?px-ix:1-(px-ix))*(dy?py-iy:1-(py-iy));
            }
            auto actual=static_cast<unsigned char*>(data)[fp.Offset+y*fp.Footprint.RowPitch+x*4+c];max_error=std::max(max_error,std::abs(actual-expected));
        }
        read->Unmap(0,nullptr);require(max_error<=1.6,"colour scale phase/edge/area oracle mismatch");
        std::cout<<"Colour area/bilinear factor="<<factor<<" odd37x29 all RGBA/edge pixels max_error="<<max_error<<" PASS\n";
        const auto* previous=scale.expanded();
        check(scale.prepare(device,w,h,rd.Format),"scale unchanged prepare");
        require(scale.expanded()==previous,"unchanged scale allocation replaced");
        check(scale.prepare(device,w+2,h+1,rd.Format),"scale resize prepare");
        const auto resized=scale.expanded()->GetDesc();
        require(resized.Width==w+2&&resized.Height==h+1,"scale resize retained stale extent");
    }
    CloseHandle(event);
}

void osr_extent_synthesis_checks(ID3D12Device* device,ID3D12CommandQueue* queue,bool asymmetric) {
    constexpr UINT guide_size=64;
    const auto texture=[&](UINT width,UINT height,DXGI_FORMAT format) {
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC description{};
        description.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        description.Width=width;description.Height=height;
        description.DepthOrArraySize=description.MipLevels=description.SampleDesc.Count=1;
        description.Format=format;
        ComPtr<ID3D12Resource> resource;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&description,
            D3D12_RESOURCE_STATE_COMMON,nullptr,IID_PPV_ARGS(&resource)),"OSR extent texture");
        return resource;
    };
    std::array colors{texture(65,64,DXGI_FORMAT_R8G8B8A8_UNORM),
        texture(63,63,DXGI_FORMAT_R8G8B8A8_UNORM)};
    auto depth=texture(guide_size,guide_size,DXGI_FORMAT_R32_FLOAT);
    auto motion=texture(guide_size,guide_size,DXGI_FORMAT_R16G16_FLOAT);
    ComPtr<ID3D12CommandAllocator> allocator;ComPtr<ID3D12GraphicsCommandList> list;
    check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
        IID_PPV_ARGS(&allocator)),"OSR upload allocator");
    check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),
        nullptr,IID_PPV_ARGS(&list)),"OSR upload list");
    std::vector<ComPtr<ID3D12Resource>> uploads;
    const std::array targets{colors[0].Get(),colors[1].Get(),depth.Get(),motion.Get()};
    for(UINT i=0;i<targets.size();++i) {
        const auto description=targets[i]->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        device->GetCopyableFootprints(&description,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_UPLOAD;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
        buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> upload;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
            D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&upload)),"OSR upload buffer");
        void* data{};check(upload->Map(0,nullptr,&data),"OSR upload map");
        std::memset(data,0,static_cast<SIZE_T>(bytes));
        for(UINT y=0;y<description.Height;++y)for(UINT x=0;x<description.Width;++x) {
            auto* pixel=static_cast<unsigned char*>(data)+footprint.Offset+
                y*footprint.Footprint.RowPitch+x*4;
            if(i<2) {const unsigned char rgba[]{80,120,160,255};std::memcpy(pixel,rgba,4);}
            else if(i==2) {const float value=.1F/3.F;std::memcpy(pixel,&value,4);}
        }
        upload->Unmap(0,nullptr);
        auto barrier=transition(targets[i],D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION source{},destination{};
        source.pResource=upload.Get();source.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint=footprint;
        destination.pResource=targets[i];destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);
        list->ResourceBarrier(1,&barrier);uploads.push_back(upload);
    }
    check(list->Close(),"OSR upload close");
    ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
    ComPtr<ID3D12Fence> fence;
    check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"OSR fence");
    const auto wait=[&](UINT64 value) {
        check(queue->Signal(fence.Get(),value),"OSR signal");
        check(fence->SetEventOnCompletion(value,nullptr),"OSR wait");
    };
    wait(1);
    xrfg::LukeAfwSynthesizer synth;
    check(synth.initialize(device,queue,{}),"native OSR initialize");
    xrfg::bridge_flight_logger().flush();
    const auto log_offset=std::filesystem::file_size(xrfg::bridge_flight_logger().log_path());
    std::vector<std::shared_ptr<DlssMotionVectorFrame>> frames;
    std::array<xrfg::LukeGuideWindow,2> windows{};
    for(unsigned n=1;n<=8;++n) {
        const unsigned eye=(n-1)%2;
        auto scene=scenes[0];
        scene.guide_width=scene.guide_height=guide_size;
        scene.jitter_x=.125F*float(int(n%3)-1);
        scene.jitter_y=-.25F*float(int(n%3)-1);
        const auto current=camera(n,scene);
        auto frame=std::make_shared<DlssMotionVectorFrame>();
        auto& guide=*frame;
        guide.stream=guide.epoch=1;guide.serial=guide.publication=n;
        guide.camera_data_valid=guide.camera_matrices_valid=true;
        guide.camera_position=current.packet.position;guide.camera_right=current.packet.right;
        guide.camera_up=current.packet.up;guide.camera_forward=current.packet.forward;
        guide.camera_view_to_clip=current.packet.projection;
        guide.camera_clip_to_view=current.packet.inverse_projection;
        guide.camera_near=current.packet.near_plane;guide.camera_far=current.packet.far_plane;
        guide.depth_inverted=guide.depth_infinite=true;
        guide.output_width=guide.output_height=guide_size;
        guide.depth_width=guide.depth_height=guide.motion_width=guide.motion_height=guide_size;
        guide.scale_x=guide.scale_y=1;
        guide.jitter_x=scene.jitter_x;guide.jitter_y=scene.jitter_y;
        guide.calibrated_game_eye_baseline=static_cast<float>(baseline);
        guide.depth=depth;guide.motion_vectors=motion;
        guide.motion_retention=std::make_shared<xrfg::DlssSnapshotRetention>();
        guide.depth_retention=std::make_shared<xrfg::DlssSnapshotRetention>();
        guide.producer_queue=queue;
        guide.verified_producer_device=guide.motion_snapshot_device=guide.depth_snapshot_device=device;
        if(n>=2)guide.previous_opposite_guide=frames[n-2];
        if(n>=3)guide.previous_same_guide=frames[n-3];
        frames.push_back(frame);
        if(n<3)continue;
        xrfg::D3D12ReprojectionView source{},destination{};
        source.pose.orientation.w=destination.pose.orientation.w=1;
        source.pose.position.x=eye?float(baseline*.5):float(-baseline*.5);
        destination.pose.position.x=-source.pose.position.x;
        const std::array angles{std::atan((asymmetric?60.F:63.F)/64.F),std::atan(63.F/64.F)};
        source.fov={-angles[eye],angles[eye],angles[eye],-angles[eye]};
        destination.fov={-angles[1-eye],angles[1-eye],angles[1-eye],-angles[1-eye]};
        const auto window=xrfg::luke_guide_window(guide.camera_view_to_clip,
            source.fov.angle_left,source.fov.angle_right,source.fov.angle_up,source.fov.angle_down,
            guide_size,guide_size,guide_size,guide_size,63,63);
        require(window.has_value(),"asymmetric OSR guide window rejected");
        if(windows[eye].width)
            require(windows[eye].width==window->width&&windows[eye].height==window->height,
                "jitter changed the same eye's depth extent");
        windows[eye]=*window;
        xrfg::embedded::AfwSettings settings{};settings.mode=3;
        settings.osr=static_cast<int>(xrfg::OsrReconstruction::full);
        std::cerr<<"Native OSR extent submission publication="<<n<<" eye="<<eye<<'\n';
        check(synth.synthesize(colors[eye].Get(),colors[1-eye].Get(),guide,source,
            destination,eye,static_cast<float>(baseline),settings),"unequal-eye native OSR submission");
        wait(n+1);
    }
    require((windows[0].width!=windows[1].width)==asymmetric&&
        (windows[0].height!=windows[1].height)==asymmetric,"unexpected fixture native depth grids");
    xrfg::bridge_flight_logger().flush();
    std::ifstream flight(xrfg::bridge_flight_logger().log_path());
    require(flight.good(),"native OSR flight log unavailable");
    flight.seekg(static_cast<std::streamoff>(log_offset));
    unsigned submissions{},histories{},motion_histories{};
    for(std::string line;std::getline(flight,line);) {
        if(line.find("op=luke_afw_stage ")==std::string::npos||
            line.find(" a=1310 ")==std::string::npos)continue;
        std::map<std::string,std::string> fields;
        std::istringstream record(line);
        for(std::string field;record>>field;) {
            const auto separator=field.find('=');
            if(separator!=std::string::npos)
                fields.emplace(field.substr(0,separator),field.substr(separator+1));
        }
        const auto flags=std::stoul(fields.at("result"));
        const auto publication=std::stoull(fields.at("b"));
        const auto previous=std::stoull(fields.at("c"));
        ++submissions;
        if(publication>=4) {
            require(previous==publication-1,
                "alternating native grids discarded the peer eye's image history");
        }
        if(publication>=5) {
            require((flags&1)!=0,"image history unavailable after both eyes were primed");
            ++histories;
        }
        if(publication>=6) {
            require((flags&2)!=0,"primed native OSR history motion unavailable");
            ++motion_histories;
        }
    }
    require(submissions==6&&histories==4&&motion_histories==3,
        "missing native OSR history regression observations");
    // Exercise actual history dispatch/readback independently of telemetry,
    // including camera-attached history, motion, and both grid directions.
    const UINT first_extent=asymmetric?60:63;
    std::array native_depths{texture(first_extent,first_extent,DXGI_FORMAT_R32_FLOAT),
        texture(63,63,DXGI_FORMAT_R32_FLOAT)};
    std::array native_motion{texture(first_extent,first_extent,DXGI_FORMAT_R16G16_FLOAT),
        texture(63,63,DXGI_FORMAT_R16G16_FLOAT)};
    check(allocator->Reset(),"native grid copy allocator");
    check(list->Reset(allocator.Get(),nullptr),"native grid copy list");
    for(unsigned eye=0;eye<2;++eye)for(unsigned kind=0;kind<2;++kind) {
        auto* src=kind?motion.Get():depth.Get();
        auto* dst=kind?native_motion[eye].Get():native_depths[eye].Get();
        auto source_barrier=transition(src,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        auto destination_barrier=transition(dst,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        const D3D12_RESOURCE_BARRIER barriers[]{source_barrier,destination_barrier};
        list->ResourceBarrier(2,barriers);
        D3D12_TEXTURE_COPY_LOCATION source{},destination{};
        source.pResource=src;source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.pResource=dst;destination.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        const auto description=dst->GetDesc();
        const D3D12_BOX box{0,0,0,static_cast<UINT>(description.Width),description.Height,1};
        list->CopyTextureRegion(&destination,0,0,0,&source,&box);
        std::swap(source_barrier.Transition.StateBefore,source_barrier.Transition.StateAfter);
        std::swap(destination_barrier.Transition.StateBefore,destination_barrier.Transition.StateAfter);
        const D3D12_RESOURCE_BARRIER restore[]{source_barrier,destination_barrier};
        list->ResourceBarrier(2,restore);
    }
    check(list->Close(),"native grid copy close");queue->ExecuteCommandLists(1,lists);wait(20);
    xrfg::OsrFullWarp warp;
    std::array<ID3D12Resource*,2> scratch{};
    for(unsigned n=1;n<=6;++n) {
        const unsigned eye=(n-1)%2,peer=1-eye;
        const auto extent=native_depths[eye]->GetDesc();
        check(warp.prepare(device,static_cast<UINT>(extent.Width),extent.Height,63,63,
            DXGI_FORMAT_R8G8B8A8_UNORM,eye),"per-eye OSR prepare");
        if(n>=2)require(warp.history(peer).publication==n-1,
            "prepare erased the other eye's native history");
        if(scratch[eye])require(warp.stereo_depth()==scratch[eye],
            "same-eye native scratch grid was reallocated");
        scratch[eye]=warp.stereo_depth();
        check(allocator->Reset(),"history oracle allocator");
        check(list->Reset(allocator.Get(),nullptr),"history oracle list");
        xrfg::OsrFullWarp::Inputs in{};
        in.source_eye=eye;in.source_depth=native_depths[eye].Get();
        in.source_color=colors[1].Get();in.source_motion=native_motion[eye].Get();
        in.source_depth_state=in.source_color_state=in.source_motion_state=D3D12_RESOURCE_STATE_COMMON;
        in.use_history=n>=2;in.use_history_motion=n>=4;
        for(unsigned i=0;i<16;i+=5) {
            in.forward_stereo[i]=in.backward_source[i]=in.forward_history[i]=1;
            in.backward_history[i]=in.forward_history_previous[i]=1;
        }
        in.forward_stereo[12]=.2F;in.backward_source[12]=-.2F;
        in.locked_depth=n%2?0.F:2.F;
        in.source_dejitter[0]=eye?-.25F:.125F;
        in.history_dejitter[0]=peer?-.25F:.125F;
        in.history_motion_scale[0]=in.history_motion_scale[1]=1;
        warp.record(device,list.Get(),in);
        xrfg::OsrFullHistory kept{};kept.publication=n;
        warp.record_history_update(list.Get(),in,kept);
        const auto resolved=warp.resolved()->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 bytes{};
        device->GetCopyableFootprints(&resolved,0,1,0,&footprint,nullptr,nullptr,&bytes);
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer{};
        buffer.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;buffer.Width=bytes;
        buffer.Height=buffer.DepthOrArraySize=buffer.MipLevels=buffer.SampleDesc.Count=1;
        buffer.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ComPtr<ID3D12Resource> readback;
        check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&buffer,
            D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback)),"history oracle readback");
        auto barrier=transition(warp.resolved(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->ResourceBarrier(1,&barrier);
        D3D12_TEXTURE_COPY_LOCATION source{},destination{};
        source.pResource=warp.resolved();source.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.pResource=readback.Get();destination.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint=footprint;
        list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
        check(list->Close(),"history oracle close");queue->ExecuteCommandLists(1,lists);
        wait(20+n);warp.commit();
        void* data{};check(readback->Map(0,nullptr,&data),"history oracle map");
        unsigned history_pixels{};
        for(UINT y=0;y<63;++y)for(UINT x=0;x<63;++x) {
            UINT tag{};
            std::memcpy(&tag,static_cast<const unsigned char*>(data)+footprint.Offset+
                y*footprint.Footprint.RowPitch+x*sizeof(UINT),sizeof(tag));
            history_pixels+=tag==2;
        }
        readback->Unmap(0,nullptr);
        if(n>=3)require(history_pixels>=63,
            "native history shaders did not fill the stereo-disocclusion band");
    }
    check(warp.prepare(device,59,60,63,63,DXGI_FORMAT_R8G8B8A8_UNORM,0),
        "changed same-eye depth grid");
    require(warp.history(0).publication==0&&warp.history(1).publication==6,
        "same-eye geometry change did not invalidate only its own history");
    check(warp.prepare(device,59,60,62,63,DXGI_FORMAT_R8G8B8A8_UNORM,0),
        "changed common color grid");
    require(warp.history(0).publication==0&&warp.history(1).publication==0,
        "color geometry change retained incompatible histories");
    require(warp.prepare(device,0,60,62,63,DXGI_FORMAT_R8G8B8A8_UNORM,0)==E_INVALIDARG,
        "empty native history grid accepted");
    std::cout<<(asymmetric?"Asymmetric":"Equal")<<" jittered depth grids; four image and three motion histories PASS\n";
}
}
