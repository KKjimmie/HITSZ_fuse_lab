#include "nfs.h"

/******************************************************************************
* SECTION: 宏定义
*******************************************************************************/
#define OPTION(t, p)        { t, offsetof(struct custom_options, p), 1 }

/******************************************************************************
* SECTION: 全局变量
*******************************************************************************/
static const struct fuse_opt option_spec[] = {		/* 用于FUSE文件系统解析参数 */
	OPTION("--device=%s", device),
	FUSE_OPT_END
};

struct nfs_super nfs_super;  				// 内存超级块
struct custom_options nfs_options;			 /* 全局选项 */
/******************************************************************************
* SECTION: FUSE操作定义
*******************************************************************************/
static struct fuse_operations operations = {
	.init = nfs_init,						 		/* mount文件系统 */		
	.destroy = nfs_destroy,				 			/* umount文件系统 */
	.mkdir = nfs_mkdir,					 			/* 建目录，mkdir */
	.getattr = nfs_getattr,				 			/* 获取文件属性，类似stat，必须完成 */
	.readdir = nfs_readdir,				 			/* 填充dentrys */
	.mknod = nfs_mknod,					 			/* 创建文件，touch相关 */
	.write = nfs_write,								/* 写入文件 */
	.read = nfs_read,								/* 读文件 */
	.utimens = nfs_utimens,				 			/* 修改时间，忽略，避免touch报错 */
	.truncate = nfs_truncate,						/* 改变文件大小 */
	.unlink = NULL,							  		/* 删除文件 */
	.rmdir	= NULL,							  		/* 删除目录， rm -r */
	.rename = NULL,							  		/* 重命名，mv */

	.open = NULL,							
	.opendir = NULL,
	.access = nfs_access
};
/******************************************************************************
* SECTION: 必做函数实现
*******************************************************************************/
/**
 * @brief 挂载（mount）文件系统
 * 
 * @param conn_info 可忽略，一些建立连接相关的信息 
 * @return void*
 */
void* nfs_init(struct fuse_conn_info * conn_info) {
	if (nfs_mount(nfs_options) != 0){
		fuse_exit(fuse_get_context()->fuse);
		return NULL;
	}
	return NULL;
}

/**
 * @brief 卸载（umount）文件系统
 * 
 * @param p 可忽略
 * @return void
 */
void nfs_destroy(void* p) {
	/* TODO: 在这里进行卸载 */
	if (nfs_umount() != 0){
		fuse_exit(fuse_get_context()->fuse);
		return;
	}

	return;
}

/**
 * @brief 创建目录
 * 
 * @param path 相对于挂载点的路径
 * @param mode 创建模式（只读？只写？），可忽略
 * @return int 0成功，否则失败
 */
int nfs_mkdir(const char* path, mode_t mode) {
	(void)mode;
	int is_find, is_root;
	char* fname;
	struct nfs_dentry* last_dentry = nfs_lookup(path, &is_find, &is_root);
	struct nfs_dentry* dentry;
	struct nfs_inode*  inode;

	if (is_find) {
		printf("Already exits.\n");
		return -NFS_ERROR_EXISTS;
	}

	if (NFS_IS_REG(last_dentry->inode)) {
		printf("Isn't a DIR.\n");
		return -NFS_ERROR_UNSUPPORTED;
	}

	fname  = nfs_get_fname(path);
	dentry = new_dentry(fname, NFS_DIR); 
	dentry->parent = last_dentry;
	inode  = nfs_alloc_inode(dentry);
	nfs_alloc_dentry(last_dentry->inode, dentry);
	
	return NFS_ERROR_NONE;
}

/**
 * @brief 获取文件或目录的属性，该函数非常重要
 * 
 * @param path 相对于挂载点的路径
 * @param nfs_stat 返回状态
 * @return int 0
 */
int nfs_getattr(const char* path, struct stat * nfs_stat) {
	/* TODO: 解析路径，获取Inode，填充nfs_stat，可参考/fs/simplefs/sfs.c的sfs_getattr()函数实现 */
	int	is_find, is_root;
	struct nfs_dentry* dentry = nfs_lookup(path, &is_find, &is_root);
	if (is_find == 0) {
		return -NFS_ERROR_NOTFOUND;
	}

	if (NFS_IS_DIR(dentry->inode)) {
		nfs_stat->st_mode = S_IFDIR | NFS_DEFAULT_PERM;
		nfs_stat->st_size = dentry->inode->dir_cnt * sizeof(struct nfs_dentry_d);
	}
	else if (NFS_IS_REG(dentry->inode)) {
		nfs_stat->st_mode = S_IFREG | NFS_DEFAULT_PERM;
		nfs_stat->st_size = dentry->inode->size;
	}
	// else if (SFS_IS_SYM_LINK(dentry->inode)) {
	// 	nfs_stat->st_mode = S_IFLNK | NFS_DEFAULT_PERM;
	// 	nfs_stat->st_size = dentry->inode->size;
	// }

	nfs_stat->st_nlink = 1;
	nfs_stat->st_uid 	 = getuid();
	nfs_stat->st_gid 	 = getgid();
	nfs_stat->st_atime   = time(NULL);
	nfs_stat->st_mtime   = time(NULL);
	nfs_stat->st_blksize = NFS_BLK_SZ();

	if (is_root) {
		nfs_stat->st_size	= 0;
		nfs_stat->st_blocks = NFS_DISK_SZ() / NFS_BLK_SZ();
		nfs_stat->st_nlink  = 2;		/* !特殊，根目录link数为2 */
	}
	return NFS_ERROR_NONE;
}

/**
 * @brief 遍历目录项，填充至buf，并交给FUSE输出
 * 
 * @param path 相对于挂载点的路径
 * @param buf 输出buffer
 * @param filler 参数讲解:
 * 
 * typedef int (*fuse_fill_dir_t) (void *buf, const char *name,
 *				const struct stat *stbuf, off_t off)
 * buf: name会被复制到buf中
 * name: dentry名字
 * stbuf: 文件状态，可忽略
 * off: 下一次offset从哪里开始，这里可以理解为第几个dentry
 * 
 * @param offset 第几个目录项？
 * @param fi 可忽略
 * @return int 0成功，否则失败
 */
int nfs_readdir(const char * path, void * buf, fuse_fill_dir_t filler, off_t offset,
			    		 struct fuse_file_info * fi) {
    /* TODO: 解析路径，获取目录的Inode，并读取目录项，利用filler填充到buf，可参考/fs/simplefs/sfs.c的sfs_readdir()函数实现 */
	int		is_find, is_root;
	int		cur_dir = offset;

	struct nfs_dentry* dentry = nfs_lookup(path, &is_find, &is_root);
	struct nfs_dentry* sub_dentry;
	struct nfs_inode* inode;
	if (is_find) {
		inode = dentry->inode;
		sub_dentry = nfs_get_dentry(inode, cur_dir);
		if (sub_dentry) {
			filler(buf, sub_dentry->fname, NULL, ++offset);
		}
		return NFS_ERROR_NONE;
	}
	return -NFS_ERROR_NOTFOUND;
}

/**
 * @brief 创建文件
 * 
 * @param path 相对于挂载点的路径
 * @param mode 创建文件的模式，可忽略
 * @param dev 设备类型，可忽略
 * @return int 0成功，否则失败
 */
int nfs_mknod(const char* path, mode_t mode, dev_t dev) {
	/* TODO: 解析路径，并创建相应的文件 */
	int	is_find, is_root;
	
	struct nfs_dentry* last_dentry = nfs_lookup(path, &is_find, &is_root);
	struct nfs_dentry* dentry;
	struct nfs_inode* inode;
	char* fname;
	
	if (is_find == 1) {
		return -NFS_ERROR_EXISTS;
	}

	fname = nfs_get_fname(path);
	
	if (S_ISREG(mode)) {
		dentry = new_dentry(fname, NFS_FILE);
	}
	else if (S_ISDIR(mode)) {
		dentry = new_dentry(fname, NFS_DIR);
	}
	else {
		dentry = new_dentry(fname, NFS_FILE);
	}
	dentry->parent = last_dentry;
	inode = nfs_alloc_inode(dentry);
	nfs_alloc_dentry(last_dentry->inode, dentry);

	return NFS_ERROR_NONE;
}

/**
 * @brief 修改时间，为了不让touch报错 
 * 
 * @param path 相对于挂载点的路径
 * @param tv 实践
 * @return int 0成功，否则失败
 */
int nfs_utimens(const char* path, const struct timespec tv[2]) {
	(void)path;
	return 0;
}


/******************************************************************************
* SECTION: 选做函数实现
*******************************************************************************/
/**
 * @brief 写入文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 写入的内容
 * @param size 写入的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 写入大小
 */
int nfs_write(const char* path, const char* buf, size_t size, off_t offset,
		        struct fuse_file_info* fi) {
	int	is_find, is_root;
	struct nfs_dentry* dentry = nfs_lookup(path, &is_find, &is_root);
	struct nfs_inode*  inode;
	
	if (is_find == 0) {
		return -NFS_ERROR_NOTFOUND;
	}

	inode = dentry->inode;
	
	if (NFS_IS_DIR(inode)) {
		return -NFS_ERROR_ISDIR;	
	}

	if (inode->size < offset) {
		return -NFS_ERROR_SEEK;
	}
	memcpy(inode->data + offset, buf, size);
	inode->size = offset + size > inode->size ? offset + size : inode->size;

	int begin_block = (int)(NFS_ROUND_DOWN(offset, NFS_BLK_SZ())/NFS_BLK_SZ());
	int end_block = (int)(NFS_ROUND_UP(offset + size, NFS_BLK_SZ())/NFS_BLK_SZ());

	// 标记脏位
	for(int i = begin_block; i < end_block && i < NFS_DATA_PER_FILE; ++i) {
		inode->dirty[i] = 1;
	}

	// 数据块的按需分配
	while (inode->size >= NFS_BLKS_SZ(inode->block_allocated)) {
		if (inode->block_allocated < NFS_DATA_PER_FILE) {
			inode->block_pointer[inode->block_allocated] = nfs_alloc_data();
			inode->block_allocated ++;
		} else {
			return -NFS_ERROR_NOSPACE;
		}
	}
	
	return size;
}

/**
 * @brief 读取文件
 * 
 * @param path 相对于挂载点的路径
 * @param buf 读取的内容
 * @param size 读取的字节数
 * @param offset 相对文件的偏移
 * @param fi 可忽略
 * @return int 读取大小
 */
int nfs_read(const char* path, char* buf, size_t size, off_t offset,
		       struct fuse_file_info* fi) {
	/* 选做 */
	int	is_find, is_root;
	struct nfs_dentry* dentry = nfs_lookup(path, &is_find, &is_root);
	struct nfs_inode*  inode;

	if (is_find == 0) {
		return -NFS_ERROR_NOTFOUND;
	}

	inode = dentry->inode;
	
	if (NFS_IS_DIR(inode)) {
		return -NFS_ERROR_ISDIR;	
	}

	if (inode->size < offset) {
		return -NFS_ERROR_SEEK;
	}

	memcpy(buf, inode->data + offset, size);

	return size;			   
}

/**
 * @brief 删除文件
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则失败
 */
int nfs_unlink(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 删除目录
 * 
 * 一个可能的删除目录操作如下：
 * rm ./tests/mnt/j/ -r
 *  1) Step 1. rm ./tests/mnt/j/j
 *  2) Step 2. rm ./tests/mnt/j
 * 即，先删除最深层的文件，再删除目录文件本身
 * 
 * @param path 相对于挂载点的路径
 * @return int 0成功，否则失败
 */
int nfs_rmdir(const char* path) {
	/* 选做 */
	return 0;
}

/**
 * @brief 重命名文件 
 * 
 * @param from 源文件路径
 * @param to 目标文件路径
 * @return int 0成功，否则失败
 */
int nfs_rename(const char* from, const char* to) {
	/* 选做 */
	return 0;
}

/**
 * @brief 打开文件，可以在这里维护fi的信息，例如，fi->fh可以理解为一个64位指针，可以把自己想保存的数据结构
 * 保存在fh中
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则失败
 */
int nfs_open(const char* path, struct fuse_file_info* fi) {
	/* 选做 */
	return 0;
}

/**
 * @brief 打开目录文件
 * 
 * @param path 相对于挂载点的路径
 * @param fi 文件信息
 * @return int 0成功，否则失败
 */
int nfs_opendir(const char* path, struct fuse_file_info* fi) {
	/* 选做 */
	return 0;
}

/**
 * @brief 改变文件大小
 * 
 * @param path 相对于挂载点的路径
 * @param offset 改变后文件大小
 * @return int 0成功，否则失败
 */
int nfs_truncate(const char* path, off_t offset) {
	/* 选做 */
	int	is_find, is_root;
	struct nfs_dentry* dentry = nfs_lookup(path, &is_find, &is_root);
	struct nfs_inode*  inode;
	
	if (is_find == 0) {
		return -NFS_ERROR_NOTFOUND;
	}
	
	inode = dentry->inode;

	if (NFS_IS_DIR(inode)) {
		return -NFS_ERROR_ISDIR;
	}

	inode->size = offset;

	return NFS_ERROR_NONE;
}


/**
 * @brief 访问文件，因为读写文件时需要查看权限
 * 
 * @param path 相对于挂载点的路径
 * @param type 访问类别
 * R_OK: Test for read permission. 
 * W_OK: Test for write permission.
 * X_OK: Test for execute permission.
 * F_OK: Test for existence. 
 * 
 * @return int 0成功，否则失败
 */
int nfs_access(const char* path, int type) {
	int	is_find, is_root;
	int is_access_ok = 0;
	struct nfs_dentry* dentry = nfs_lookup(path, &is_find, &is_root);
	struct nfs_inode*  inode;

	switch (type)
	{
	case R_OK:
		is_access_ok = 1;
		break;
	case F_OK:
		if (is_find) {
			is_access_ok = 1;
		}
		break;
	case W_OK:
		is_access_ok = 1;
		break;
	case X_OK:
		is_access_ok = 1;
		break;
	default:
		break;
	}
	return is_access_ok ? NFS_ERROR_NONE : -NFS_ERROR_ACCESS;
}	
/******************************************************************************
* SECTION: FUSE入口
*******************************************************************************/
int main(int argc, char **argv)
{
    int ret;
	struct fuse_args args = FUSE_ARGS_INIT(argc, argv);

	nfs_options.device = strdup("/home/kjm/ddriver");

	if (fuse_opt_parse(&args, &nfs_options, option_spec, NULL) == -1)
		return -1;
	
	ret = fuse_main(args.argc, args.argv, &operations, NULL);
	fuse_opt_free_args(&args);
	return ret;
}