NAME=nntppost

sudo rm -rf rootfs
mkdir rootfs
sh ./scripts/create_rootfs.sh $NAME rootfs/
cp $NAME rootfs/
sudo docker build -t localhost/$NAME .
sudo docker save localhost/$NAME | gzip > $NAME-docker-image.tar.gz
sudo docker rmi localhost/$NAME
