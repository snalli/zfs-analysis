/*pseudo driver used for corruption */
#include <sys/types.h>
#include <sys/file.h>
#include <sys/errno.h>
#include <sys/open.h>
#include <sys/cred.h>
#include <sys/cmn_err.h>
#include <sys/modctl.h>
#include <sys/conf.h>
#include <sys/stat.h>
#include <sys/fcntl.h> 
#include <sys/ddi.h>
#include <sys/sunddi.h>
#include <sys/sunldi.h>


typedef struct corrupt {
        char            * ptr_name;      /* The pointer getting squished  -DO NOT USE */
        uint64_t        new_value;      /* New value of the pointer.        */
        uint32_t        blockno;        /* Block where the pointer resides. */
        uint32_t        offset;         /* Offset */
        uint32_t        bit;            /* Bit */
        struct corrupt_t *     nxt;
} corrupt_t;

corrupt_t * crpt_t;

enum lyr_op
{
  lyr_load = 3393

};


typedef struct zfs_lyr_state {
  ldi_handle_t    lh;
  ldi_ident_t     li;
  dev_info_t      *dip;
  minor_t     minor[2];
  int         flags;
  kmutex_t    lock;
  //adding state specific to block driver
  kmutex_t mu;
  kcondvar_t cv;
  struct buf* bp;        //outstanding request
  int	(*orig_b_iodone)(struct buf *); //original endio function, to be called after layered driver's endio
  int busy;
} zfs_lyr_state_t;

#define LYR_OPENED      0x1     /* lh is valid */
#define LYR_IDENTED     0x2     /* li is valid */

static int zfs_lyr_info(dev_info_t *, ddi_info_cmd_t, void *, void **);
static int zfs_lyr_attach(dev_info_t *, ddi_attach_cmd_t);
static int zfs_lyr_detach(dev_info_t *, ddi_detach_cmd_t);

static int zfs_lyr_open(dev_t *, int, int, cred_t *);
static int zfs_lyr_close(dev_t, int, int, cred_t *);
static int zfs_lyr_strategy(struct buf *);
//static void zfs_lyr_start(zfs_lyr_state_t *);
static int zfs_lyr_ioctl(dev_t, int, intptr_t, int , cred_t *, int *);
static int zfs_lyr_prop_op(dev_t, dev_info_t *, ddi_prop_op_t , int , char *, caddr_t , int *);
static int zfs_lyr_iodone(struct buf *);
//static int zfs_lyr_write(dev_t, struct uio *, cred_t *);

static void *zfs_lyr_statep;

static struct cb_ops zfs_lyr_cb_ops = {
    zfs_lyr_open,       /* open */
    zfs_lyr_close,      /* close */
    zfs_lyr_strategy,   /* strategy */
    nodev,      /* print */
    nodev,      /* dump */
    nodev,      /* read */

    nodev,      /* write */
    zfs_lyr_ioctl,      /* ioctl */
    nodev,      /* devmap */
    nodev,      /* mmap */
    nodev,      /* segmap */
    nochpoll,       /* poll */
    zfs_lyr_prop_op,    /* prop_op */
    NULL,       /* streamtab  */
    D_NEW | D_MP,   /* cb_flag */
    CB_REV,     /* cb_rev */
    nodev,      /* aread */
    nodev       /* awrite */
};

static struct dev_ops zfs_lyr_dev_ops = {
    DEVO_REV,       /* devo_rev, */
    0,          /* refcnt  */
    zfs_lyr_info,       /* getinfo */
    nulldev,    /* identify */
    nulldev,    /* probe */
    zfs_lyr_attach,     /* attach */
    zfs_lyr_detach,     /* detach */
    nodev,      /* reset */
    &zfs_lyr_cb_ops,    /* cb_ops */
    NULL,       /* bus_ops */
    NULL        /* power */
};

static struct modldrv modldrv = {
    &mod_driverops,
    "LDI ZFS driver",
    &zfs_lyr_dev_ops
};

static struct modlinkage modlinkage = {
    MODREV_1,
    &modldrv,
    NULL
};


int
_init(void)
{
    int rv;
    cmn_err(CE_NOTE, "Inside zfs_lyr _init\n");
    if ((rv = ddi_soft_state_init(&zfs_lyr_statep, sizeof (zfs_lyr_state_t),
        0)) != 0) {
      cmn_err(CE_WARN, "zfs_lyr _init: soft state init failed\n");
      return (rv);
    }

    if ((rv = mod_install(&modlinkage)) != 0) {
      cmn_err(CE_WARN, "zfs_lyr _init: mod_install failed\n");
      goto FAIL;
    }

    return (rv);
    /*NOTEREACHED*/
FAIL:
    ddi_soft_state_fini(&zfs_lyr_statep);
    return (rv);
}


int
_info(struct modinfo *modinfop)
{
    return (mod_info(&modlinkage, modinfop));
}


int
_fini(void)
{
    int rv;
    cmn_err(CE_NOTE, "Inside zfs_lyr _fini\n");
    if ((rv = mod_remove(&modlinkage)) != 0) {
        return(rv);
    }

    ddi_soft_state_fini(&zfs_lyr_statep);

    return (rv);
}

/*
 * 1:1 mapping between minor number and instance
 */
static int
zfs_lyr_info(dev_info_t *dip, ddi_info_cmd_t infocmd, void *arg, void **result)
{
    int inst;
    minor_t minor;
    zfs_lyr_state_t *statep;
    char *myname = "zfs_lyr_info";

    cmn_err(CE_NOTE, "Inside %s\n",myname);

    minor = getminor((dev_t)arg);
    inst = minor;
    switch (infocmd) {
    case DDI_INFO_DEVT2DEVINFO:
        statep = ddi_get_soft_state(zfs_lyr_statep, inst);
        if (statep == NULL) {
            cmn_err(CE_WARN, "%s: get soft state "
                "failed on inst %d\n", myname, inst);
            return (DDI_FAILURE);
        }
        *result = (void *)statep->dip;
        break;
    case DDI_INFO_DEVT2INSTANCE:
        *result = (void *)inst;
        break;
    default:
        break;
    }

    return (DDI_SUCCESS);
}


static int
zfs_lyr_attach(dev_info_t *dip, ddi_attach_cmd_t cmd)
{
    int inst;
    zfs_lyr_state_t *statep;
    char *myname = "zfs_lyr_attach";

    cmn_err(CE_NOTE, "Inside %s\n",myname);
    switch (cmd) {
    case DDI_ATTACH:
        inst = ddi_get_instance(dip);

        if (ddi_soft_state_zalloc(zfs_lyr_statep, inst) != DDI_SUCCESS) {
            cmn_err(CE_WARN, "%s: ddi_soft_state_zallac failed "
                "on inst %d\n", myname, inst);
            goto FAIL;
        }

        statep = (zfs_lyr_state_t *)ddi_get_soft_state(zfs_lyr_statep, inst);
        if (statep == NULL) {
            cmn_err(CE_WARN, "%s: ddi_get_soft_state failed on "
                "inst %d\n", myname, inst);
            goto FAIL;
        }
        statep->dip = dip;
        statep->minor[0] = inst;
        statep->minor[1] = inst + 1;
        cmn_err(CE_WARN,"Instance number in attach is %d\n",inst);  
        if (ddi_create_minor_node(dip, "zfsminor1", S_IFBLK, statep->minor[0],
            DDI_PSEUDO, 0) != DDI_SUCCESS) {
            cmn_err(CE_WARN, "%s: ddi_create_minor_node failed on "
                "inst %d\n", myname, inst);
            goto FAIL;
        }
        if (ddi_create_minor_node(dip, "zfsminor2", S_IFCHR, statep->minor[1],
            DDI_PSEUDO, 0) != DDI_SUCCESS) {
            cmn_err(CE_WARN, "%s: ddi_create_minor_node failed on "
                "inst %d\n", myname, inst);
            goto FAIL;
        }
 
        mutex_init(&statep->lock, NULL, MUTEX_DRIVER, NULL);
	mutex_init(&statep->mu, NULL, MUTEX_DRIVER, NULL);
	cv_init(&statep->cv, NULL, CV_DRIVER, NULL);

	statep->bp = NULL;
	statep->busy = 0;
	statep->orig_b_iodone = NULL;
        return (DDI_SUCCESS);

    case DDI_RESUME:
    case DDI_PM_RESUME:
    default:
        break;
    }
    return (DDI_FAILURE);
    /*NOTREACHED*/
FAIL:
    ddi_soft_state_free(zfs_lyr_statep, inst);
    ddi_remove_minor_node(dip, NULL);
    return (DDI_FAILURE);
}


static int
zfs_lyr_detach(dev_info_t *dip, ddi_detach_cmd_t cmd)
{
    int inst;
    zfs_lyr_state_t *statep;
    char *myname = "zfs_lyr_detach";

     cmn_err(CE_NOTE, "Inside %s\n",myname);
    inst = ddi_get_instance(dip);
    statep = ddi_get_soft_state(zfs_lyr_statep, inst);
    if (statep == NULL) {
        cmn_err(CE_WARN, "%s: get soft state failed on "
            "inst %d\n", myname, inst);
        return (DDI_FAILURE);
    }
    if (statep->dip != dip) {
        cmn_err(CE_WARN, "%s: soft state does not match devinfo "
            "on inst %d\n", myname, inst);
        return (DDI_FAILURE);
    }

    switch (cmd) {
    case DDI_DETACH:
        mutex_destroy(&statep->lock);
	mutex_destroy(&statep->mu);
	cv_destroy(&statep->cv);
        ddi_soft_state_free(zfs_lyr_statep, inst);
        ddi_remove_minor_node(dip, NULL);
        return (DDI_SUCCESS);
    case DDI_SUSPEND:
    case DDI_PM_SUSPEND:
    default:
        break;
    }
    return (DDI_FAILURE);
}

/*
 * on this driver's open, we open the target specified by a property and store
 * the layered handle and ident in our soft state.  a good target would be
 * "/dev/console" or more interestingly, a pseudo terminal as specified by the
 * tty command
 */
/*ARGSUSED*/
static int
zfs_lyr_open(dev_t *devtp, int oflag, int otyp, cred_t *credp)
{
    int rv; 
    //inst = getminor(*devtp);
    int inst = 1; 
    zfs_lyr_state_t *statep;
    char *myname = "zfs_lyr_open";
    dev_info_t *dip;
    char *zfs_lyr_targ = NULL;

    int64_t drv_prop64;
    uint_t value;
    uint64_t sizep;

    cmn_err(CE_NOTE, "Inside %s*******SYNC for instance %d dev_t is %u \n",myname, inst,*devtp);
    //cmn_err(CE_WARN, "open is %d \n",otyp );
    statep = (zfs_lyr_state_t *)ddi_get_soft_state(zfs_lyr_statep, inst); //hack inst is 1 
    if (statep == NULL) {
        cmn_err(CE_WARN, "%s: ddi_get_soft_state failed on "
            "inst %d\n", myname, inst);
        return (EIO);
    }
    dip = statep->dip;

    /*
     * our target device to open should be specified by the "zfs_lyr_targ"
     * string property, which should be set in this driver's .conf file
     */
    if (ddi_prop_lookup_string(DDI_DEV_T_ANY, dip, DDI_PROP_NOTPROM,
        "zfs_lyr_targ", &zfs_lyr_targ) != DDI_PROP_SUCCESS) {
        cmn_err(CE_WARN, "%s: ddi_prop_lookup_string failed on "
            "inst %d\n", myname, inst);
        return (EIO);
    }

    /*
     * since we only have one pair of lh's and li's available, we don't
     * allow multiple on the same instance
     */
    mutex_enter(&statep->lock);
    if (statep->flags & (LYR_OPENED | LYR_IDENTED)) {
        cmn_err(CE_WARN, "%s: Allowing multiple layered opens or idents "
            "from inst %d %l\n", myname, inst, *devtp);
        //mutex_exit(&statep->lock);
        //ddi_prop_free(zfs_lyr_targ);
        //return (EIO);

       rv = 0;
    }
  
    if (1 == getminor(*devtp)) 
    {
        rv = ldi_ident_from_dev(*devtp, &statep->li);
        if (rv != 0) {
            cmn_err(CE_WARN, "%s: ldi_ident_from_dev failed on inst %d\n",
                myname, inst);
            goto FAIL;
        }
        statep->flags |= LYR_IDENTED;

        rv = ldi_open_by_name(zfs_lyr_targ, FREAD | FWRITE, credp, &statep->lh,
            statep->li);
        if (rv != 0) {
            cmn_err(CE_WARN, "%s: ldi_open_by_name failed on inst %d\n",
                myname, inst);
            goto FAIL;
        }
        statep->flags |= LYR_OPENED;

        cmn_err(CE_CONT, "\n%s: opened target '%s' successfully on inst %d\n",
            myname, zfs_lyr_targ, inst);
        rv = 0;

        /* adding code to check if Nblocks/nblocks property is set */
        if (ldi_prop_exists(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM, "Nblocks")) {
            drv_prop64 = ldi_prop_get_int64(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM,"Nblocks", 0);
            sizep = (uint64_t)ldbtob((uint64_t)drv_prop64);
            cmn_err(CE_CONT,"\n%s: sucessfully got Nblocks=%d\n", myname, sizep); 
        }
    
        if (ldi_prop_exists(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM, "nblocks")) {
            value = ldi_prop_get_int(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM,"nblocks", 0);
            sizep = (uint64_t)ldbtob(value);
            cmn_err(CE_CONT,"\n%s: sucessfully got Nblocks=%d\n", myname, sizep); 
        }
        /* end */
    }

FAIL:
    /* cleanup on error */
    if (rv != 0) {
        if (statep->flags & LYR_OPENED)
            (void)ldi_close(statep->lh, FREAD | FWRITE, credp);
        if (statep->flags & LYR_IDENTED)
            ldi_ident_release(statep->li);
        statep->flags &= ~(LYR_OPENED | LYR_IDENTED);
    }
    mutex_exit(&statep->lock);

    if (zfs_lyr_targ != NULL)
        ddi_prop_free(zfs_lyr_targ);
    return (rv);
}


/*
 * on this driver's close, we close the target indicated by the lh member
 * in our soft state and release the ident, li as well.  in fact, we MUST do
 * both of these at all times even if close yields an error because the
 * device framework effectively closes the device, releasing all data
 * associated with it and simply returning whatever value the target's
 * close(9E) returned.  therefore, we must as well.
 */
/*ARGSUSED*/
static int
zfs_lyr_close(dev_t devt, int oflag, int otyp, cred_t *credp)
{
    int rv = 0, inst = getminor(devt);
    zfs_lyr_state_t *statep;
    char *myname = "zfs_lyr_close";
    inst = 1; //hack
    cmn_err(CE_NOTE, "Inside %s with dev_t=%l\n",myname,devt);

    statep = (zfs_lyr_state_t *)ddi_get_soft_state(zfs_lyr_statep, inst);  
    if (statep == NULL) {
        cmn_err(CE_WARN, "%s: ddi_get_soft_state failed on "
            "inst %d\n", myname, inst);
        return (EIO);
    }

    mutex_enter(&statep->lock);

    if (1 == getminor(devt))
    { 
        rv = ldi_close(statep->lh, FREAD | FWRITE, credp);
        if (rv != 0) {
           cmn_err(CE_WARN, "%s: ldi_close failed on inst %d, but will ",
           "continue to release ident\n", myname, inst);
        }
        ldi_ident_release(statep->li);
        if (rv == 0) {
            cmn_err(CE_CONT, "\n%s: closed target successfully on "
            "inst %d\n", myname, inst);
        }
        statep->flags &= ~(LYR_OPENED | LYR_IDENTED);
    }
    mutex_exit(&statep->lock);
    return (rv);
}

static int
zfs_lyr_strategy(struct buf *bp)
{
  minor_t inst;
  zfs_lyr_state_t *statep;
  char *myname = "zfs_lyr_strategy";
  unsigned char *buf_addr;
  static int once = 0;
  int i;

  cmn_err(CE_NOTE, "Inside %s *******SYNC with Mu and Cv\n",myname);

  inst = getminor(bp->b_edev);

  cmn_err(CE_NOTE, "%s: inst = %d\n", myname, inst);
  statep = (zfs_lyr_state_t *)ddi_get_soft_state(zfs_lyr_statep, inst);

  if(statep == NULL) {
     cmn_err(CE_WARN, "%s: ddi_get_soft_state failed on "
            "inst %d\n", myname, inst);
     bioerror(bp, ENXIO);
     biodone(bp);
     return (0);
  }
  /* validate the transfer request */
  //FIX ME: validate the upper limit by setting statep->Nblocks to the appropriate size of the device in 512 byte blocks - currently I don't know how the layered driver can find out this property for the real device
  if(bp->b_blkno < 0)
    {
      bioerror(bp, EINVAL);
      biodone(bp);
      return (0);
    }
  
  bp_mapin(bp); 
    
  mutex_enter(&statep->mu);
  while(statep->busy){
    cv_wait(&statep->cv, &statep->mu);
  }
  statep->busy = 1; 
  
  buf_addr = (unsigned char *)(bp->b_un.b_addr);
  if(buf_addr == NULL)
    {
      cmn_err(CE_WARN, "%s: Data Buffer pointer is null!\n", myname);
    }
  else
    {
      if(bp->b_flags & B_WRITE)
	{
	  cmn_err(CE_WARN, " -> %s: Buffer for write is: \n", myname);
	  //for(i=0; i< bp->b_bcount; i++)
	   // cmn_err(CE_NOTE, " %c ", *buf_addr++);
	}

    }
  
 
  /* Switch its endio to point to zfs_lyr_iodone */
  statep->orig_b_iodone = bp->b_iodone;
  bp->b_iodone = zfs_lyr_iodone;
  cmn_err(CE_NOTE, "%s: Switched endio to point to zfs_lyr_endio.\n",myname);

  mutex_exit(&statep->mu);

  //cmn_err(CE_WARN, "%s: Could not pass on strategy request to target device.\n", myname);
  //else
    cmn_err(CE_NOTE, "%s: Passing strategy request to target device....\n",myname);  

  int rv = ldi_strategy(statep->lh, bp);
 
  //We are corrupting here
  if(bp->b_flags & B_READ)  
  {
    cmn_err(CE_WARN,"Reading block %d, count %d\n", bp->b_blkno, bp->b_bcount);
    if (crpt_t!=NULL)
      if ((bp->b_blkno <= crpt_t->blockno) && (bp->b_blkno + bp->b_bcount/512 >= crpt_t->blockno)) 
      {
         buf_addr = (unsigned char *)(bp->b_un.b_addr); 
         if (buf_addr != NULL)
         {
            int k; 
            buf_addr = buf_addr + (crpt_t->blockno - bp->b_blkno)*512 + crpt_t->offset; 
            //buf_addr = buf_addr + (crpt_t->blockno - bp->b_blkno)*512 ; 
           for (k=0;k<512;k++)  
           {  
                printf ("Changed %c to %c at %d offset", *(buf_addr + k), crpt_t->new_value, crpt_t->offset +k);
               *(buf_addr + k) = crpt_t->new_value; 
            } 
            //*(uint64_t *)buf_addr = crpt_t->new_value; 
            //*(uint64_t *)(buf_addr+8) = crpt_t->new_value; 
            //*(uint64_t *)(buf_addr+16) = crpt_t->new_value; 

            cmn_err(CE_WARN,"Corrupted block %d at offset %d\n", crpt_t->blockno, crpt_t->offset);
         }
      }       
  } 

  return rv;
}

static int
zfs_lyr_ioctl(dev_t dev, int cmd, intptr_t arg, int mode, cred_t *credp, int *rvalp)
{
  minor_t inst;
  zfs_lyr_state_t *statep;
  char *myname = "zfs_lyr_ioctl";
  int ret;
  
  cmn_err(CE_NOTE, "Inside the ioctl routine with cmd %d and ddi_model_convert_from %d.\n",cmd, ddi_model_convert_from(mode&FMODELS) );

  //inst = getminor(dev);
  inst = 1; 
  cmn_err(CE_NOTE, "%s: inst = %d\n", myname, inst);
  statep = (zfs_lyr_state_t *)ddi_get_soft_state(zfs_lyr_statep, inst); //hack inst is 1 

  if(statep == NULL) {
     cmn_err(CE_WARN, "%s: ddi_get_soft_state failed on "
            "inst %d\n", myname, inst);
     return (0);
  }
 
  /* Process the ioctl from our user space application. */
  switch (cmd)
  {
    case lyr_load :
     crpt_t = kmem_zalloc(sizeof(corrupt_t), KM_SLEEP); 
     if (crpt_t == NULL)
       return DDI_FAILURE; 
     if (ddi_copyin((void *)arg, crpt_t, sizeof(corrupt_t), mode))
      return DDI_FAILURE;
    
     if (crpt_t!=NULL)  
          cmn_err(CE_WARN,"Called for corrupting block %d, offset %d\n",crpt_t->blockno, crpt_t->offset);
     return DDI_SUCCESS;
  }
 
  cmn_err(CE_NOTE, "%s: Calling real device's ioctl on inst %d\n", myname, inst);
  cmn_err(CE_NOTE, "%s: cmd = %d, mode = %d \n", myname, cmd, mode);
  ret = ldi_ioctl(statep->lh,cmd, arg, mode, credp, rvalp);
  if(ret == 0)
    cmn_err(CE_NOTE, "%s: ioctl to real device succeeded\n", myname);
  else
    cmn_err(CE_NOTE, "%s: ioctl with cmd = %d to real device FAILED with status=%d!\n", myname, cmd, ret);
  
  return ret;
}

static int
zfs_lyr_prop_op(dev_t dev, dev_info_t *dip, ddi_prop_op_t prop_op, int flags, char *name, caddr_t valuep, int *lengthp)
{
  zfs_lyr_state_t *statep;
  minor_t inst;
  char *myname = "zfs_lyr_prop_op";
  int ret;
  
  int64_t drv_prop64;
  uint_t value;
  uint64_t sizep;

  
  inst = 1; // hack here, orignal was inst = getminor(dev);

  cmn_err(CE_NOTE, "%s: inst = %d\n", myname, inst);
  statep = (zfs_lyr_state_t *)ddi_get_soft_state(zfs_lyr_statep, inst);

  if(statep == NULL) {
     cmn_err(CE_WARN, "%s: ddi_get_soft_state failed on "
            "inst %d\n", myname, inst);
     return (0);
  }
  cmn_err(CE_NOTE, "%s: Calling real device's prop_op on inst %d\n", myname, inst);
  ret = ldi_prop_op(statep->lh, prop_op, flags, name, valuep, lengthp);
  if(ret == 0)
    cmn_err(CE_NOTE, "%s: prop_op %d passed to real device successfully\n", myname, prop_op);
  else
    cmn_err(CE_NOTE, "%s: passing prop_op %d to real device FAILED!\n", myname, prop_op);

  if(ret != 0)
    {
      if (ldi_prop_exists(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM, "Nblocks")) {
	drv_prop64 = ldi_prop_get_int64(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM,"Nblocks", 0);
	sizep = (uint64_t)ldbtob((uint64_t)drv_prop64);
	cmn_err(CE_CONT,"\n%s: sucessfully got Nblocks=%d\n", myname, sizep); 
      }
      
      if (ldi_prop_exists(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM, "nblocks")) {
	value = ldi_prop_get_int(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM,"nblocks", 0);
	sizep = (uint64_t)ldbtob(value);
	cmn_err(CE_CONT,"\n%s: sucessfully got nblocks=%d\n", myname, sizep); 
      }
      ret = ddi_prop_op_nblocks(dev, dip, prop_op, flags, name, valuep, lengthp, sizep);

      if (ldi_prop_exists(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM, "Size")) {
	drv_prop64 = ldi_prop_get_int64(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM,"Size", 0);
	//sizep = (uint64_t)ldbtob((uint64_t)drv_prop64);
	sizep = (uint64_t)drv_prop64;
	cmn_err(CE_CONT,"\n%s: sucessfully got Size=%d\n", myname, sizep); 
      }
      else
	cmn_err(CE_CONT, "\n%s: Prop 'Size' does not exist for target\n", myname);
      
      if (ldi_prop_exists(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM, "size")) {
	value = ldi_prop_get_int(statep->lh,DDI_PROP_DONTPASS | DDI_PROP_NOTPROM,"size", 0);
	//sizep = (uint64_t)ldbtob(value);
	sizep = (uint64_t)value;
	cmn_err(CE_CONT,"\n%s: sucessfully got size=%d\n", myname, sizep); 
      }
      else
	cmn_err(CE_CONT, "\n%s: Prop 'size' does not exist for target\n", myname);

      ret = ddi_prop_op_size(dev, dip, prop_op, flags, name, valuep, lengthp, sizep);
      }
  return ret;

}
  

static int
zfs_lyr_iodone(struct buf* bp)
{
  zfs_lyr_state_t *statep;
  minor_t inst;
  char *myname = "zfs_lyr_iodone";
  unsigned char *buf_addr;
  int i;
  
  cmn_err(CE_NOTE, "Inside %s\n",myname);

  if((bp->b_resid != 0) && ((bp->b_flags & B_ERROR)==0)) //as long as the entire tranfer is not complete, or has not resulted in an errror, just don't do anything
    {
      return 0;
    }

  if(bp == NULL)
    cmn_err(CE_WARN, "%s: found NULL buffer \n", myname);

  inst = getminor(bp->b_edev);
  cmn_err(CE_NOTE, "%s: inst = %d\n", myname, inst);
  statep = (zfs_lyr_state_t *)ddi_get_soft_state(zfs_lyr_statep, inst);
  if(statep == NULL) {
    bioerror(bp, ENXIO);
    biodone(bp);
    cmn_err(CE_WARN, "%s: Failed to get state in layered driver's endio..endio hanging possible..\n",myname);
    return (0);
  }
  
  mutex_enter(&statep->mu);
  ASSERT(statep->bp == bp);

  if(bp->b_flags & B_ERROR)
    {
      cmn_err(CE_WARN, "%s: Could not complete I/O, error=%d \n",myname, bp->b_error);
      bioerror(bp, bp->b_error);
    }
    
  if(bp->b_flags & B_READ)
  {
    cmn_err(CE_NOTE, "%s: Done a READ\n", myname);
    buf_addr = (unsigned char *)(bp->b_un.b_addr);
    if(buf_addr == NULL)
      {
	cmn_err(CE_WARN, "%s: Data Buffer pointer is null!\n", myname);
      }
    else
      {
	cmn_err(CE_NOTE, "%s: Buffer for read: \n", myname);
	for(i=0; i< bp->b_bcount && i< 20; i++)
	  cmn_err(CE_NOTE, " %c ", *buf_addr++);
      }
    }

  if(bp->b_flags & B_WRITE)
    cmn_err(CE_NOTE, "%s: Done a WRITE\n", myname);

  // Restore the original iodone routine 
  bp->b_iodone = statep->orig_b_iodone;
  cmn_err(CE_NOTE, "%s: Calling the original iodone routine, which was saved for %d\n",myname, inst);
  biodone(bp);

  //Reset the busy flag and wake up any waiters 
  statep->busy = 0;
  cv_signal(&statep->cv);
  statep->bp = NULL;
  cmn_err(CE_NOTE, "%s: After original iodone, reset busy flag and woke up waiters\n", myname);
  
  mutex_exit(&statep->mu);
  
  return (0);
    
}
