#ifndef _LINUX_PATH_H
#define _LINUX_PATH_H

struct dentry;
struct vfsmount;

struct path {
	struct vfsmount *mnt; /*文件系统的挂载结构*/
	struct dentry *dentry;  /*文件系统mnt的根目录项*/
};

extern void path_get(struct path *);
extern void path_put(struct path *);

#endif  /* _LINUX_PATH_H */
