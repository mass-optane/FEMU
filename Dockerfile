FROM ubuntu:latest
LABEL maintainer="Junliang HU <jlhu@cse.cuhk.edu.hk>"
# ENV http_proxy=http://proxy.cse.cuhk.edu.hk:8000
# ENV https_proxy=$http_proxy
# ENV ftp_proxy=$http_proxy
ARG DEBIAN_FRONTEND=noninteractive
# ENV TZ=Asia/Hong_Kong
ENV FEMU_PREFIX=/femu
ENV PATH=$FEMU_PREFIX/bin:$PATH
# apt proxy
# RUN sed -i 's/archive.ubuntu.com/ftp.cuhk.edu.hk\/pub\/Linux/g' /etc/apt/sources.list \
#     && sed -i 's/security.ubuntu.com/ftp.cuhk.edu.hk\/pub\/Linux/g' /etc/apt/sources.list \
#     && echo "Acquire::http::Proxy \"$http_proxy\";\nAcquire::https::Proxy \"$https_proxy/\";" > /etc/apt/apt.conf.d/99proxy
# wget proxy
# building dependencies
RUN apt-get update \
    && apt-get install -y build-essential gdb pkg-config git libglib2.0-dev libfdt-dev libpixman-1-dev zlib1g-dev libaio-dev libnuma-dev ninja-build cpu-checker libcap2-bin \
    && rm -rf /var/lib/apt/lists/* 
# RUN echo "https_proxy = $https_proxy\nhttp_proxy = $http_proxy\nftp_proxy = $ftp_proxy" >> /etc/wgetrc
# building and installing FEMU
ADD femu.tar femu-build 
RUN cd femu-build \
    && ./configure --enable-kvm --target-list=x86_64-softmmu --disable-werror --disable-strip --prefix=$FEMU_PREFIX \
    && make -j$(nproc) && make install \
    && cd .. && rm -rf femu-build
RUN setcap cap_ipc_lock=eip $(realpath $(which qemu-system-x86_64)) && getcap $(realpath $(which qemu-system-x86_64)) 
CMD ["bash"]

# make docker run act as if we are directly executing qemu-system-x86_64
# ENTRYPOINT ["qemu-system-x86_64"]
# if no arguements are passed, print help messages
# CMD ["--help"]
