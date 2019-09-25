#ifndef COM_UNIT_HPP
#define COM_UNIT_HPP

#include <vector>
#include <unordered_map>
#include "Grid/map_grid.hpp"
#include "VCluster/VCluster.hpp"
#include "Space/SpaceBox.hpp"
#include "util/mathutil.hpp"
#include "Iterators/grid_dist_id_iterator_dec.hpp"
#include "Iterators/grid_dist_id_iterator.hpp"
#include "Iterators/grid_dist_id_iterator_sub.hpp"
#include "grid_dist_key.hpp"
#include "NN/CellList/CellDecomposer.hpp"
#include "util/object_util.hpp"
#include "memory/ExtPreAlloc.hpp"
#include "VTKWriter/VTKWriter.hpp"
#include "Packer_Unpacker/Packer.hpp"
#include "Packer_Unpacker/Unpacker.hpp"
#include "Decomposition/CartDecomposition.hpp"
#include "data_type/aggregate.hpp"
#include "hdf5.h"
#include "grid_dist_id_comm.hpp"
#include "HDF5_wr/HDF5_wr.hpp"
#include "SparseGrid/SparseGrid.hpp"
#ifdef __NVCC__
#include "SparseGridGpu/SparseGridGpu.hpp"
#include "cuda/grid_dist_id_kernels.cuh"
#include "Grid/cuda/grid_dist_id_iterator_gpu.cuh"
#endif


//! Internal ghost box sent to construct external ghost box into the other processors
template<unsigned int dim>
struct Box_fix
{
	//! Box in global unit
	Box<dim,size_t> bx;
	//! In which sector live the box
	comb<dim> cmb;
	//! Global id of the internal ghost box
	size_t g_id;
	//! from which sub-domain this internal ghost box is generated (or with which sub-domain is overlapping)
	size_t r_sub;
};

#define NO_GDB_EXT_SWITCH 0x1000

/*! \brief This is a distributed grid
 *
 * Implementation of a distributed grid the decomposition is geometrical, grid
 * is splitted across several processor
 *
 * \param dim Dimensionality of the grid
 * \param St Type of space where the grid is living
 * \param T object the grid is storing
 * \param Decomposition Class that decompose the grid for example CartDecomposition
 * \param Mem Is the allocator
 * \param device type of base structure is going to store the data
 *
 * ### Create a distributed grid and access it
 * \snippet grid_dist_id_unit_test.cpp Create and access a distributed grid
 * ### Synchronize the ghosts and check the information
 * \snippet grid_dist_id_unit_test.cpp Synchronize the ghost and check the information
 * ### Create and access a distributed grid for complex structures
 * \snippet grid_dist_id_unit_test.cpp Create and access a distributed grid complex
 * ### Synchronize a distributed grid for complex structures
 * \snippet grid_dist_id_unit_test.cpp Synchronized distributed grid complex
 * ### Usage of a grid dist iterator sub
 * \snippet grid_dist_id_iterators_unit_tests.hpp Usage of a sub_grid iterator
 * ### Construct two grid with the same decomposition
 * \snippet grid_dist_id_unit_test.cpp Construct two grid with the same decomposition
 *
 */
template<unsigned int dim,
		 typename St,
		 typename T,
		 typename Decomposition = CartDecomposition<dim,St>,
		 typename Memory=HeapMemory ,
		 typename device_grid=grid_cpu<dim,T> >
class grid_dist_id : public grid_dist_id_comm<dim,St,T,Decomposition,Memory,device_grid>
{
	//! Domain
	Box<dim,St> domain;

	//! Ghost expansion
	Ghost<dim,St> ghost;

	//! Ghost expansion
	Ghost<dim,long int> ghost_int;

	//! Local grids
	mutable openfpm::vector<device_grid> loc_grid;

	//! Old local grids
	mutable openfpm::vector<device_grid> loc_grid_old;

	//! Space Decomposition
	Decomposition dec;

	//! gdb_ext markers
	//! In the case where the grid is defined everywhere
	//! gdb_ext_marker is useless and so is empty
	//! in the case we have a grid defined on a smaller set
	//! of boxes gbd_ext_markers indicate the division across
	//! subdomains. For example Sub-domain 0 produce 2 grid
	//! Sub-domain 1 produce 3 grid Sub-domain 2 produce 2 grid
	//! Sub-domain 3 produce 1 grid
	//! gdb_ext_markers contain 0,2,5,7,8
	openfpm::vector<size_t> gdb_ext_markers;

	//! Extension of each grid: Domain and ghost + domain
	openfpm::vector<GBoxes<device_grid::dims>> gdb_ext;

	//! Global gdb_ext
	mutable openfpm::vector<GBoxes<device_grid::dims>> gdb_ext_global;

	//! Extension of each old grid (old): Domain and ghost + domain
	openfpm::vector<GBoxes<device_grid::dims>> gdb_ext_old;

	//! Size of the grid on each dimension
	size_t g_sz[dim];

	//! Structure that divide the space into cells
	CellDecomposer_sm<dim,St,shift<dim,St>> cd_sm;

	//! size of the insert pool on gpu
	size_t gpu_insert_pool_size;

	//! Communicator class
	Vcluster<> & v_cl;

	//! properties names
	openfpm::vector<std::string> prp_names;

	//! It map a global ghost id (g_id) to the external ghost box information
	//! It is unique across all the near processor
	std::unordered_map<size_t,size_t> g_id_to_external_ghost_box;

	/*! Link a received external ghost box to the linked eg_box.
	 * When the grid is defined everywhere for each received external ghost box
	 * exists one eg_box linked to it that contain the information
	 * on how to transfer the information to the associated sub-domain grid.
	 * Unfortunately when we specify where the grid is defined, a received external
	 * ghost box can be linked to multiple sub-domain grids (one sub-domain can have multiple
	 * sub grids).
	 * So in standard situation (grid defined everywhere)
	 * a received external ghost box is linked to a single
	 * eg_box entry and eb_gid_list play mainly no role.
	 * (play no role but must be filled ghost_get expect this structure to be
	 * filled consistently, it will be clear later how to do it in this case).
	 * When the grid is not defined everywhere a received ghost box can be linked
	 * to multiple external ghost boxes. (Like in figure)
	 *
	 * \verbatim
  +--------------------------------------------+------
  |        Sub-domain                          | Another sub-domain
  |          +------+           +---------+    |
  |          |      |           |         |    |
  |          |      |           |         |    |
  |          |      |           |         |    |
  |          |  3   |           |    4    |    |
  |   empty  |      |    empty  |         |    |
  |          |      |           |         |    |
  |          |      |           |         |    |
  |          |      |           |         |    |                         1
  |          |      |           |         |    |
+-+-----+----+------+-----------+---------+----+-----+-----   Processor bound
        |***##########*********#############****|****|
        |                                            |                   0
        |                                            |
        |                                            |
        |                  9                         |
        |                                            |
        |                                            |
        |                                            |
        +--------------------------------------------+

	 * \endverbatim
	 *
	 * As we can see here the grid number 9 on processo 0 has an internal ghost box
	 * The internal ghost-box is sent to processor 1 and is a received external
	 * ghost box. This external ghost box is partially shared in two separated grids.
	 * It is important to note that 3 and 4 are grid defined externally and are
	 * not defined by the sub-domain border. It is important also to note that the sub-domain
	 * granularity in processor 1 define the granularity of the internal ghost box in
	 * processor 0 and consequently every external ghost box in processor 1 is linked
	 *  uniquely with one internal ghost box in processor 0. On the other hand if we have
	 *  a secondary granularity define by external boxes like 3 and 4 this is not anymore
	 *  true and one internal ghost box in 0 can be linked with multiple grids.
	 *  The granularity of the space division is different from the granularity of where
	 *  the grid is defined. Space decomposition exist independently from the data-structure
	 *  and can be shared across multiple data-structure this mean that cannot be redefined
	 *  based on where is the grid definitions.
	 * The internal ghost box could be redefined in order to respect the granularity.
	 * We do not do this for 3 main reason.
	 *
	 * 1) The definition box must be communicated across processors.
	 * 2) An interprocessor global-id link must be established with lower sub-domain
	 *    granularty
	 * 3) Despite the points * are not linked, but must be anyway sent
	 *    to processor 1, this mean that make not too much sense to increase
	 *    the granularity in advance on processor 0, but it is better receive
	 *    the information an than solve the lower granularity locally
	 *    on processor 1
	 *
	 */
	openfpm::vector<e_box_multi<dim>> eb_gid_list;

	//! It map a global ghost id (g_id) to the internal ghost box information
	//! (is unique for processor), it is not unique across all the near processor
	openfpm::vector<std::unordered_map<size_t,size_t>> g_id_to_internal_ghost_box;

	//! Receiving size
	openfpm::vector<size_t> recv_sz;

	//! Receiving buffer for particles ghost get
	openfpm::vector<HeapMemory> recv_mem_gg;

	//! Grid informations object
	grid_sm<dim,T> ginfo;

	//! Grid informations object without type
	grid_sm<dim,void> ginfo_v;

	//! Set of boxes that define where the grid is defined
	openfpm::vector<Box<dim,long int>> bx_def;

	//! Indicate if we have to use bx_def to define the grid
	bool use_bx_def = false;

	//! Indicate if the local internal ghost box has been initialized
	bool init_local_i_g_box = false;

	//! Indicate if the local external ghost box has been initialized
	bool init_local_e_g_box = false;

	//! Flag that indicate if the external ghost box has been initialized
	bool init_e_g_box = false;

	//! Flag that indicate if the internal ghost box has been initialized
	bool init_i_g_box = false;

	//! Flag that indicate if the internal and external ghost box has been fixed
	bool init_fix_ie_g_box = false;

	//! Internal ghost boxes in grid units
	openfpm::vector<ip_box_grid<dim>> ig_box;

	//! External ghost boxes in grid units
	openfpm::vector<ep_box_grid<dim>> eg_box;

	//! Local internal ghost boxes in grid units
	openfpm::vector<i_lbox_grid<dim>> loc_ig_box;

	//! Local external ghost boxes in grid units
	openfpm::vector<e_lbox_grid<dim>> loc_eg_box;

	//! Number of sub-sub-domain for each processor
	size_t v_sub_unit_factor = 64;

	/*! \brief Call-back to allocate buffer to receive incoming objects (external ghost boxes)
	 *
	 * \param msg_i message size required to receive from i
	 * \param total_msg message size to receive from all the processors
	 * \param total_p the total number of processor want to communicate with you
	 * \param i processor id
	 * \param ri request id (it is an id that goes from 0 to total_p, and is unique
	 *           every time message_alloc is called)
	 * \param ptr void pointer parameter for additional data to pass to the call-back
	 *
	 */
	static void * msg_alloc_external_box(size_t msg_i ,size_t total_msg, size_t total_p, size_t i, size_t ri, void * ptr)
	{
		grid_dist_id<dim,St,T,Decomposition,Memory,device_grid> * g = static_cast<grid_dist_id<dim,St,T,Decomposition,Memory,device_grid> *>(ptr);

		g->recv_sz.resize(g->dec.getNNProcessors());
		g->recv_mem_gg.resize(g->dec.getNNProcessors());

		// Get the local processor id
		size_t lc_id = g->dec.ProctoID(i);

		// resize the receive buffer
		g->recv_mem_gg.get(lc_id).resize(msg_i);
		g->recv_sz.get(lc_id) = msg_i;

		return g->recv_mem_gg.get(lc_id).getPointer();
	}

	/*! \brief this function is for optimization of the ghost size
	 *
	 * Because the decomposition work in continuum and discrete ghost is
	 *  converted in continuum, in some case continuum ghost because of
	 *  rounding-off error can produce ghost bigger than the discrete selected
	 *   one. This function adjust for this round-off error
	 *
	 * \param sub_id sub-domain id
	 * \param sub_domain_other the other sub-domain
	 * \param ib internal ghost box to adjust
	 *
	 */
	void set_for_adjustment(size_t sub_id,
							const Box<dim,St> & sub_domain_other,
							const comb<dim> & cmb,
							Box<dim,long int> & ib,
							Ghost<dim,long int> & g)
	{
		if (g.isInvalidGhost() == true || use_bx_def == true)
		{return;}

		Box<dim,long int> sub_domain;

		if (use_bx_def == false)
		{
			sub_domain = gdb_ext.get(sub_id).Dbox;
			sub_domain += gdb_ext.get(sub_id).origin;
		}

		// Convert from SpaceBox<dim,St> to SpaceBox<dim,long int>
		Box<dim,long int> sub_domain_other_exp = cd_sm.convertDomainSpaceIntoGridUnits(sub_domain_other,dec.periodicity());

		// translate sub_domain_other based on cmb
		for (size_t i = 0 ; i < dim ; i++)
		{
			if (cmb.c[i] == 1)
			{
				sub_domain_other_exp.setLow(i,sub_domain_other_exp.getLow(i) - ginfo.size(i));
				sub_domain_other_exp.setHigh(i,sub_domain_other_exp.getHigh(i) - ginfo.size(i));
			}
			else if (cmb.c[i] == -1)
			{
				sub_domain_other_exp.setLow(i,sub_domain_other_exp.getLow(i) + ginfo.size(i));
				sub_domain_other_exp.setHigh(i,sub_domain_other_exp.getHigh(i) + ginfo.size(i));
			}
		}

		sub_domain_other_exp.enlarge(g);
		if (sub_domain_other_exp.Intersect(sub_domain,ib) == false)
		{
			for (size_t i = 0 ; i < dim ; i++)
			{ib.setHigh(i,ib.getLow(i) - 1);}
		}
	}



	/*! \brief Create per-processor internal ghost boxes list in grid units and g_id_to_external_ghost_box
	 *
	 */
	void create_ig_box()
	{
		// temporal vector used for computation
		openfpm::vector_std<result_box<dim>> ibv;

		if (init_i_g_box == true)	return;

		// Get the grid info
		auto g = cd_sm.getGrid();

		g_id_to_internal_ghost_box.resize(dec.getNNProcessors());

		// Get the number of near processors
		for (size_t i = 0 ; i < dec.getNNProcessors() ; i++)
		{
			ig_box.add();
			auto&& pib = ig_box.last();

			pib.prc = dec.IDtoProc(i);
			for (size_t j = 0 ; j < dec.getProcessorNIGhost(i) ; j++)
			{
				// Get the internal ghost boxes and transform into grid units
				::Box<dim,St> ib_dom = dec.getProcessorIGhostBox(i,j);
				::Box<dim,long int> ib = cd_sm.convertDomainSpaceIntoGridUnits(ib_dom,dec.periodicity());

				size_t sub_id = dec.getProcessorIGhostSub(i,j);
				size_t r_sub = dec.getProcessorIGhostSSub(i,j);

				auto & n_box = dec.getNearSubdomains(dec.IDtoProc(i));

				set_for_adjustment(sub_id,
						           n_box.get(r_sub),dec.getProcessorIGhostPos(i,j),
						           ib,ghost_int);

				// Here we intersect the internal ghost box with the definition boxes
				// this operation make sense when the grid is not defined in the full
				// domain and we have to intersect the internal ghost box with all the box
				// that define where the grid is defined
				bx_intersect<dim>(bx_def,use_bx_def,ib,ibv);

				for (size_t k = 0 ; k < ibv.size() ; k++)
				{
					// Check if ib is valid if not it mean that the internal ghost does not contain information so skip it
					if (ibv.get(k).bx.isValid() == false)
					{continue;}

					// save the box and the sub-domain id (it is calculated as the linearization of P1)
					::Box<dim,size_t> cvt = ibv.get(k).bx;

					i_box_id<dim> bid_t;
					bid_t.box = cvt;
					bid_t.g_id = dec.getProcessorIGhostId(i,j) | (k) << 52;

					bid_t.sub = convert_to_gdb_ext(dec.getProcessorIGhostSub(i,j),
							                       ibv.get(k).id,
												   gdb_ext,
												   gdb_ext_markers);

					bid_t.cmb = dec.getProcessorIGhostPos(i,j);
					bid_t.r_sub = dec.getProcessorIGhostSSub(i,j);
					pib.bid.add(bid_t);

					g_id_to_internal_ghost_box.get(i)[bid_t.g_id | (k) << 52 ] = pib.bid.size()-1;
				}
			}
		}

		init_i_g_box = true;
	}


	/*! \brief Create per-processor internal ghost box list in grid units
	 *
	 */
	void create_eg_box()
	{
		// Get the grid info
		auto g = cd_sm.getGrid();

		if (init_e_g_box == true)	return;

		// Here we collect all the calculated internal ghost box in the sector different from 0 that this processor has

		openfpm::vector<size_t> prc;
		openfpm::vector<size_t> prc_recv;
		openfpm::vector<size_t> sz_recv;
		openfpm::vector<openfpm::vector<Box_fix<dim>>> box_int_send(dec.getNNProcessors());
		openfpm::vector<openfpm::vector<Box_fix<dim>>> box_int_recv;

		for(size_t i = 0 ; i < dec.getNNProcessors() ; i++)
		{
			for (size_t j = 0 ; j < ig_box.get(i).bid.size() ; j++)
			{
				box_int_send.get(i).add();
				box_int_send.get(i).last().bx = ig_box.get(i).bid.get(j).box;
				box_int_send.get(i).last().g_id = ig_box.get(i).bid.get(j).g_id;
				box_int_send.get(i).last().r_sub = ig_box.get(i).bid.get(j).r_sub;
				box_int_send.get(i).last().cmb = ig_box.get(i).bid.get(j).cmb;
			}
			prc.add(dec.IDtoProc(i));
		}

		v_cl.SSendRecv(box_int_send,box_int_recv,prc,prc_recv,sz_recv);

		eg_box.resize(dec.getNNProcessors());

		for (size_t i = 0 ; i < eg_box.size() ; i++)
		{eg_box.get(i).prc = dec.IDtoProc(i);}

		for (size_t i = 0 ; i < box_int_recv.size() ; i++)
		{
			size_t pib_cnt = 0;
			size_t p_id = dec.ProctoID(prc_recv.get(i));
			auto&& pib = eg_box.get(p_id);
			pib.prc = prc_recv.get(i);

			eg_box.get(p_id).recv_pnt = 0;
			eg_box.get(p_id).n_r_box = box_int_recv.get(i).size();

			// For each received internal ghost box
			for (size_t j = 0 ; j < box_int_recv.get(i).size() ; j++)
			{
				size_t vol_recv = box_int_recv.get(i).get(j).bx.getVolumeKey();

				eg_box.get(p_id).recv_pnt += vol_recv;
				size_t send_list_id = box_int_recv.get(i).get(j).r_sub;

				if (use_bx_def == true)
				{
					// First we understand if the internal ghost box sent intersect
					// some local extended sub-domain.

					// eb_gid_list, for an explanation check the declaration
					eb_gid_list.add();
					eb_gid_list.last().e_id = p_id;

					// Now we have to check if a received external ghost box intersect one
					// or more sub-grids
					for (size_t k = 0 ; k < gdb_ext.size() ; k++)
					{
						Box<dim,long int> bx = gdb_ext.get(k).GDbox;
						bx += gdb_ext.get(k).origin;

						Box<dim,long int> output;
						Box<dim,long int> flp_i = flip_box(box_int_recv.get(i).get(j).bx,box_int_recv.get(i).get(j).cmb,ginfo);

						// it intersect one sub-grid
						if (bx.Intersect(flp_i,output))
						{
							// link

							size_t g_id = box_int_recv.get(i).get(j).g_id;
							add_eg_box<dim>(k,box_int_recv.get(i).get(j).cmb,output,
									g_id,
									gdb_ext.get(k).origin,
									box_int_recv.get(i).get(j).bx.getP1(),
									pib.bid);

							eb_gid_list.last().eb_list.add(pib.bid.size() - 1);

							g_id_to_external_ghost_box[g_id] = eb_gid_list.size() - 1;
						}
					}

					// now we check if exist a full match across the full intersected
					// ghost parts

					bool no_match = true;
					for (size_t k = 0 ; k < eb_gid_list.last().eb_list.size() ; k++)
					{
						size_t eb_id = eb_gid_list.last().eb_list.get(k);

						if (pib.bid.get(eb_id).g_e_box == box_int_recv.get(i).get(j).bx)
						{
							// full match found

							eb_gid_list.last().full_match = eb_id;
							no_match = false;

							break;
						}
					}

					// This is the case where a full match has not been found. In this case we
					// generate an additional gdb_ext and local grid with only external ghost

					if (no_match == true)
					{
						// Create a grid with the same size of the external ghost

						size_t sz[dim];
						for (size_t s = 0 ; s < dim ; s++)
						{sz[s] = box_int_recv.get(i).get(j).bx.getHigh(s) - box_int_recv.get(i).get(j).bx.getLow(s) + 1;}

						// Add an unlinked gdb_ext
						// An unlinked gdb_ext is an empty domain with only a external ghost
						// part
						Box<dim,long int> output = flip_box(box_int_recv.get(i).get(j).bx,box_int_recv.get(i).get(j).cmb,ginfo);

						GBoxes<dim> tmp;
						tmp.GDbox = box_int_recv.get(i).get(j).bx;
						tmp.GDbox -= tmp.GDbox.getP1();
						tmp.origin = output.getP1();
						for (size_t s = 0 ; s < dim ; s++)
						{
							// we set an invalid box, there is no-domain
							tmp.Dbox.setLow(s,0);
							tmp.Dbox.setHigh(s,-1);
						}
						tmp.k = (size_t)-1;
						gdb_ext.add(tmp);

						// create the local grid

						loc_grid.add();
						loc_grid.last().resize(sz);

						// Add an external ghost box

						size_t g_id = box_int_recv.get(i).get(j).g_id;
						add_eg_box<dim>(gdb_ext.size()-1,box_int_recv.get(i).get(j).cmb,output,
								g_id,
								gdb_ext.get(gdb_ext.size()-1).origin,
								box_int_recv.get(i).get(j).bx.getP1(),
								pib.bid);

						// now we map the received ghost box to the information of the
						// external ghost box created
						eb_gid_list.last().full_match = pib.bid.size() - 1;
						eb_gid_list.last().eb_list.add(pib.bid.size() - 1);
						g_id_to_external_ghost_box[g_id] = eb_gid_list.size() - 1;
					}

					// now we create lr_e_box

					size_t fm = eb_gid_list.last().full_match;
					size_t sub_id = pib.bid.get(fm).sub;

					for ( ; pib_cnt < pib.bid.size() ; pib_cnt++)
					{
						pib.bid.get(pib_cnt).lr_e_box -= gdb_ext.get(sub_id).origin;
					}

				}
				else
				{
					// Get the list of the sent sub-domains
					// and recover the id of the sub-domain from
					// the sent list
					const openfpm::vector<size_t> & s_sub = dec.getSentSubdomains(p_id);

					size_t sub_id = s_sub.get(send_list_id);

					e_box_id<dim> bid_t;
					bid_t.sub = sub_id;
					bid_t.cmb = box_int_recv.get(i).get(j).cmb;
					bid_t.cmb.sign_flip();
					::Box<dim,long int> ib = flip_box(box_int_recv.get(i).get(j).bx,box_int_recv.get(i).get(j).cmb,ginfo);
					bid_t.g_e_box = ib;
					bid_t.g_id = box_int_recv.get(i).get(j).g_id;
					// Translate in local coordinate
					Box<dim,long int> tb = ib;
					tb -= gdb_ext.get(sub_id).origin;
					bid_t.l_e_box = tb;

					pib.bid.add(bid_t);
					eb_gid_list.add();
					eb_gid_list.last().eb_list.add(pib.bid.size()-1);
					eb_gid_list.last().e_id = p_id;
					eb_gid_list.last().full_match = pib.bid.size()-1;

					g_id_to_external_ghost_box[bid_t.g_id] = eb_gid_list.size()-1;
				}
			}
		}

		init_e_g_box = true;
	}

	/*! \brief Create local internal ghost box in grid units
	 *
	 */
	void create_local_ig_box()
	{
		openfpm::vector_std<result_box<dim>> ibv;

		// Get the grid info
		auto g = cd_sm.getGrid();

		if (init_local_i_g_box == true)	return;

		// Get the number of sub-domains
		for (size_t i = 0 ; i < dec.getNSubDomain() ; i++)
		{
			loc_ig_box.add();
			auto&& pib = loc_ig_box.last();

			for (size_t j = 0 ; j < dec.getLocalNIGhost(i) ; j++)
			{
				if (use_bx_def == true)
				{
					// Get the internal ghost boxes and transform into grid units
					::Box<dim,St> ib_dom = dec.getLocalIGhostBox(i,j);
					::Box<dim,long int> ib = cd_sm.convertDomainSpaceIntoGridUnits(ib_dom,dec.periodicity());

					// Here we intersect the internal ghost box with the definition boxes
					// this operation make sense when the grid is not defined in the full
					// domain and we have to intersect the internal ghost box with all the box
					// that define where the grid is defined
					bx_intersect<dim>(bx_def,use_bx_def,ib,ibv);

					for (size_t k = 0 ; k < ibv.size() ; k++)
					{
						// Check if ib is valid if not it mean that the internal ghost does not contain information so skip it
						if (ibv.get(k).bx.isValid() == false)
						{continue;}

						pib.bid.add();
						pib.bid.last().box = ibv.get(k).bx;

						pib.bid.last().sub_gdb_ext = convert_to_gdb_ext(i,
																ibv.get(k).id,
																gdb_ext,
																gdb_ext_markers);

						pib.bid.last().sub = dec.getLocalIGhostSub(i,j);

						// It will be filled later
						pib.bid.last().cmb = dec.getLocalIGhostPos(i,j);
					}
				}
				else
				{
					// Get the internal ghost boxes and transform into grid units
					::Box<dim,St> ib_dom = dec.getLocalIGhostBox(i,j);
					::Box<dim,long int> ib = cd_sm.convertDomainSpaceIntoGridUnits(ib_dom,dec.periodicity());

					// Check if ib is valid if not it mean that the internal ghost does not contain information so skip it
					if (ib.isValid() == false)
						continue;

					size_t sub_id = i;
					size_t r_sub = dec.getLocalIGhostSub(i,j);

					set_for_adjustment(sub_id,dec.getSubDomain(r_sub),
							           dec.getLocalIGhostPos(i,j),ib,ghost_int);

					// Check if ib is valid if not it mean that the internal ghost does not contain information so skip it
					if (ib.isValid() == false)
					continue;

					pib.bid.add();
					pib.bid.last().box = ib;
					pib.bid.last().sub = dec.getLocalIGhostSub(i,j);
					pib.bid.last().k.add(dec.getLocalIGhostE(i,j));
					pib.bid.last().cmb = dec.getLocalIGhostPos(i,j);
					pib.bid.last().sub_gdb_ext = i;
				}
			}

			if (use_bx_def == true)
			{
				// unfortunately boxes that define where the grid is located can generate
				// additional internal ghost boxes

				for (size_t j = gdb_ext_markers.get(i) ; j < gdb_ext_markers.get(i+1) ; j++)
				{
					// intersect within box in the save sub-domain

					for (size_t k = gdb_ext_markers.get(i) ; k < gdb_ext_markers.get(i+1) ; k++)
					{
						if (j == k)	{continue;}

						// extend k and calculate the internal ghost box
						Box<dim,long int> bx_e =  gdb_ext.get(k).GDbox;
						bx_e += gdb_ext.get(k).origin;
						Box<dim,long int> bx = gdb_ext.get(j).Dbox;
						bx += gdb_ext.get(j).origin;

						Box<dim,long int> output;
						if (bx.Intersect(bx_e, output) == true)
						{
							pib.bid.add();

							pib.bid.last().box = output;

							pib.bid.last().sub_gdb_ext = j;
							pib.bid.last().sub = i;

							// these ghost always in the quadrant zero
							pib.bid.last().cmb.zero();

						}
					}
				}
			}

		}


		init_local_i_g_box = true;
	}

	/*! \brief Create per-processor external ghost boxes list in grid units
	 *
	 */
	void create_local_eg_box()
	{
		// Get the grid info
		auto g = cd_sm.getGrid();

		if (init_local_e_g_box == true)	return;

		loc_eg_box.resize(dec.getNSubDomain());

		// Get the number of sub-domain
		for (size_t i = 0 ; i < dec.getNSubDomain() ; i++)
		{
			for (size_t j = 0 ; j < loc_ig_box.get(i).bid.size() ; j++)
			{
				long int volume_linked = 0;

				size_t le_sub = loc_ig_box.get(i).bid.get(j).sub;
				auto & pib = loc_eg_box.get(le_sub);

				if (use_bx_def == true)
				{

					// We check if an external local ghost box intersect one
					// or more sub-grids
					for (size_t k = 0 ; k < gdb_ext.size() ; k++)
					{
						Box<dim,long int> bx = gdb_ext.get(k).Dbox;
						bx += gdb_ext.get(k).origin;

						Box<dim,long int> gbx = gdb_ext.get(k).GDbox;
						gbx += gdb_ext.get(k).origin;

						Box<dim,long int> output;
						Box<dim,long int> flp_i = flip_box(loc_ig_box.get(i).bid.get(j).box,loc_ig_box.get(i).bid.get(j).cmb,ginfo);

						bool intersect_domain = bx.Intersect(flp_i,output);
						bool intersect_gdomain = gbx.Intersect(flp_i,output);

						// it intersect one sub-grid
						if (intersect_domain == false && intersect_gdomain == true)
						{
							// fill the link variable
							loc_ig_box.get(i).bid.get(j).k.add(pib.bid.size());
							size_t s = loc_ig_box.get(i).bid.get(j).k.last();

							comb<dim> cmb = loc_ig_box.get(i).bid.get(j).cmb;
							cmb.sign_flip();

							add_loc_eg_box(le_sub,
										   loc_ig_box.get(i).bid.get(j).sub_gdb_ext,
										   j,
										   k,
										   pib.bid,
										   output,
										   cmb);


							volume_linked += pib.bid.last().ebox.getVolumeKey();
						}
					}
				}
				else
				{
					size_t k = loc_ig_box.get(i).bid.get(j).sub;
					auto & pib = loc_eg_box.get(k);

					size_t s = loc_ig_box.get(i).bid.get(j).k.get(0);
					pib.bid.resize(dec.getLocalNEGhost(k));

					pib.bid.get(s).ebox = flip_box(loc_ig_box.get(i).bid.get(j).box,loc_ig_box.get(i).bid.get(j).cmb,ginfo);
					pib.bid.get(s).sub = dec.getLocalEGhostSub(k,s);
					pib.bid.get(s).cmb = loc_ig_box.get(i).bid.get(j).cmb;
					pib.bid.get(s).cmb.sign_flip();
					pib.bid.get(s).k = j;
					pib.bid.get(s).initialized = true;
					pib.bid.get(s).sub_gdb_ext = k;
				}
			}
		}

		init_local_e_g_box = true;
	}


	/*! \brief Check the grid has a valid size
	 *
	 * \param g_sz size of the grid
	 *
	 */
	inline void check_size(const size_t (& g_sz)[dim])
	{
		for (size_t i = 0 ; i < dim ; i++)
		{
			if (g_sz[i] < 2)
				std::cerr << "Error: " << __FILE__ << ":" << __LINE__ << " distributed grids with size smaller than 2 are not supported\n";
		}
	}

	/*! \brief Create the grids on memory
	 *
	 * \param bx_def Where the grid is defined
	 * \param use_bx_def use the array that define where the grid is defined
	 *
	 */
	void Create(openfpm::vector<Box<dim,long int>> & bx_def,
			    const Ghost<dim,long int> & g,
			    bool use_bx_def)
	{
		// create gdb
		create_gdb_ext<dim,Decomposition>(gdb_ext,gdb_ext_markers,dec,cd_sm,bx_def,g,use_bx_def);

		size_t n_grid = gdb_ext.size();

		// create local grids for each hyper-cube
		loc_grid.resize(n_grid);

		// Size of the grid on each dimension
		size_t l_res[dim];

		// Allocate the grids
		for (size_t i = 0 ; i < n_grid ; i++)
		{
			SpaceBox<dim,long int> sp_tg = gdb_ext.get(i).GDbox;

			// Get the size of the local grid
			// The boxes indicate the extension of the index the size
			// is this extension +1
			// for example a 1D box (interval) from 0 to 3 in one dimension have
			// the points 0,1,2,3 = so a total of 4 points
			for (size_t j = 0 ; j < dim ; j++)
			{l_res[j] = (sp_tg.getHigh(j) >= 0)?(sp_tg.getHigh(j)+1):0;}

			// Set the dimensions of the local grid
			loc_grid.get(i).resize(l_res);
		}
	}


    /*! \brief Initialize the Cell decomposer of the grid enforcing perfect overlap of the cells
	 *
	 * \param cd_old the CellDecomposer we are trying to mach
	 * \param ext extension of the domain
	 *
	 */
	inline void InitializeCellDecomposer(const CellDecomposer_sm<dim,St,shift<dim,St>> & cd_old, const Box<dim,size_t> & ext)
	{
		// Initialize the cell decomposer
		cd_sm.setDimensions(cd_old,ext);
	}

    /*! \brief Initialize the Cell decomposer of the grid
	 *
	 * \param g_sz Size of the grid
	 * \param bc boundary conditions
	 *
	 */
	inline void InitializeCellDecomposer(const size_t (& g_sz)[dim], const size_t (& bc)[dim])
	{
		// check that the grid has valid size
		check_size(g_sz);

		// get the size of the cell decomposer
		size_t c_g[dim];
		getCellDecomposerPar<dim>(c_g,g_sz,bc);

		// Initialize the cell decomposer
		cd_sm.setDimensions(domain,c_g,0);
	}

	/*! \brief Initialize the grid
	 *
	 * \param g_sz Global size of the grid
	 * \param bc boundary conditions
	 *
	 */
	inline void InitializeDecomposition(const size_t (& g_sz)[dim], const size_t (& bc)[dim])
	{
		// fill the global size of the grid
		for (size_t i = 0 ; i < dim ; i++)	{this->g_sz[i] = g_sz[i];}

		// Get the number of processor and calculate the number of sub-domain
		// for decomposition
		size_t n_proc = v_cl.getProcessingUnits();
		size_t n_sub = n_proc * v_sub_unit_factor;

		// Calculate the maximum number (before merging) of sub-domain on
		// each dimension
		size_t div[dim];
		for (size_t i = 0 ; i < dim ; i++)
		{div[i] = openfpm::math::round_big_2(pow(n_sub,1.0/dim));}

		// Create the sub-domains
		dec.setParameters(div,domain,bc,ghost);
		dec.decompose();
	}

	/*! \brief Initialize the grid
	 *
	 * \param g_sz Global size of the grid
	 *
	 */
	inline void InitializeStructures(const size_t (& g_sz)[dim])
	{
		// an empty
		openfpm::vector<Box<dim,long int>> empty;

		// Ghost zero
		Ghost<dim,long int> zero;

		InitializeStructures(g_sz,empty,zero,false);
	}

	/*! \brief Initialize the grid
	 *
	 * \param g_sz Global size of the grid
	 * \param g ghost extension of the grid in integer unit
	 * \param bx set of boxes that define where is defined the grid
	 *
	 */
	inline void InitializeStructures(const size_t (& g_sz)[dim],
			                         openfpm::vector<Box<dim,long int>> & bx,
									 const Ghost<dim,long int> & g,
									 bool use_bx_def)
	{
		// fill the global size of the grid
		for (size_t i = 0 ; i < dim ; i++)	{this->g_sz[i] = g_sz[i];}

		// Create local grid
		Create(bx,g,use_bx_def);
	}

	// Ghost as integer
	Ghost<dim,long int> gint = Ghost<dim,long int>(0);

protected:

	/*! \brief Given a local sub-domain i with a local grid Domain + ghost return the part of the local grid that is domain
	 *
	 * \param i sub-domain
	 *
	 * \return the Box defining the domain in the local grid
	 *
	 */
	Box<dim,size_t> getDomain(size_t i)
	{
		return gdb_ext.get(i).Dbox;
	}

	/*! \brief Convert a ghost from grid point units into continus space
	 *
	 * \param gd Ghost in continuous space
	 * \param cd_sm CellDecomposer of the grid
	 *
	 * \return the ghost in continuous unit
	 *
	 */
	static inline Ghost<dim,float> convert_ghost(const Ghost<dim,long int> & gd, const CellDecomposer_sm<dim,St,shift<dim,St>> & cd_sm)
	{
		Ghost<dim,float> gc;

		// get the grid spacing
		Box<dim,St> sp = cd_sm.getCellBox();

		// enlarge 0.001 of the spacing
		sp.magnify_fix_P1(1.1);

		// set the ghost
		for (size_t i = 0 ; i < dim ; i++)
		{
			gc.setLow(i,gd.getLow(i)*(sp.getHigh(i)));
			gc.setHigh(i,gd.getHigh(i)*(sp.getHigh(i)));
		}

		return gc;
	}

	/*! \brief Set the minimum number of sub-domain per processor
	 *
	 * \param n_sub
	 *
	 */
	void setDecompositionGranularity(size_t n_sub)
	{
		this->v_sub_unit_factor = n_sub;
	}

	void reset_ghost_structures()
	{
		g_id_to_internal_ghost_box.clear();
		ig_box.clear();
		init_i_g_box = false;

		eg_box.clear();
		eb_gid_list.clear();
		init_e_g_box = false;

		init_local_i_g_box = false;
		loc_ig_box.clear();

		init_local_e_g_box = false;
		loc_eg_box.clear();
	}

public:

	//! Which kind of grid the structure store
	typedef device_grid d_grid;

	//! Decomposition used
	typedef Decomposition decomposition;

	//! value_type
	typedef T value_type;

	//! Type of space
	typedef St stype;

	//! Type of Memory
	typedef Memory memory_type;

	//! Type of device grid
	typedef device_grid device_grid_type;

	//! Number of dimensions
	static const unsigned int dims = dim;

	/*! \brief Get the domain where the grid is defined
	 *
	 * \return the domain of the grid
	 *
	 */
	inline const Box<dim,St> getDomain() const
	{
		return domain;
	}

	/*! \brief Get the point where it start the origin of the grid of the sub-domain i
	 *
	 * \param i sub-domain
	 *
	 * \return the point
	 *
	 */
	Point<dim,St> getOffset(size_t i)
	{
		return pmul(Point<dim,St>(gdb_ext.get(i).origin), cd_sm.getCellBox().getP2()) + getDomain().getP1();
	}

    /*! \brief Get the spacing of the grid in direction i
     *
     * \param i dimension
     *
     * \return the spacing
     *
     */
    inline St spacing(size_t i) const
    {
    	return cd_sm.getCellBox().getHigh(i);
    }

	/*! \brief Return the total number of points in the grid
	 *
	 * \return number of points
	 *
	 */
	size_t size() const
	{
		return ginfo_v.size();
	}

	/*! \brief set the background value
	 *
	 * You can use this function make sense in case of sparse in case of dense
	 * it does nothing
	 *
	 */
	void setBackgroundValue(T & bv)
	{
		setBackground_impl<T,decltype(loc_grid)> func(bv,loc_grid);
		boost::mpl::for_each_ref< boost::mpl::range_c<int,0,T::max_prop>>(func);
	}

	/*! \brief set the background value
	 *
	 * You can use this function make sense in case of sparse in case of dense
	 * it does nothing
	 *
	 */
	template<unsigned int p>
	void setBackgroundValue(const typename boost::mpl::at<typename T::type,boost::mpl::int_<p>>::type & bv)
	{
		for (size_t i = 0 ; i < loc_grid.size() ; i++)
		{loc_grid.get(i).template setBackgroundValue<p>(bv);}
	}

	/*! \brief Return the local total number of points inserted in the grid
	 *
	 * in case of dense grid it return the number of local points, in case of
	 * sparse it return the number of inserted points
	 *
	 * \return number of points
	 *
	 */
	size_t size_local_inserted() const
	{
		size_t lins = 0;

		for (size_t i = 0 ; i < loc_grid.size() ; i++)
		{lins += loc_grid.get(i).size_inserted();}

		return lins;
	}

	/*! \brief Return the total number of points in the grid
	 *
	 * \param i direction
	 *
	 * \return number of points on direction i
	 *
	 */
	size_t size(size_t i) const
	{
		return ginfo_v.size(i);
	}

	/*! \brief Default Copy constructor on this class make no sense and is unsafe, this definition disable it
	 *
	 * \param g grid to copy
	 *
	 */
	grid_dist_id(const grid_dist_id<dim,St,T,Decomposition,Memory,device_grid> & g)
	:grid_dist_id_comm<dim,St,T,Decomposition,Memory,device_grid>(g),
	 domain(g.domain),
	 ghost(g.ghost),
	 ghost_int(g.ghost_int),
	 loc_grid(g.loc_grid),
	 loc_grid_old(g.loc_grid_old),
	 dec(g.dec),
	 gdb_ext_markers(g.gdb_ext_markers),
	 gdb_ext(g.gdb_ext),
	 gdb_ext_global(g.gdb_ext_global),
	 gdb_ext_old(g.gdb_ext_old),
	 cd_sm(g.cd_sm),
	 v_cl(g.v_cl),
	 prp_names(g.prp_names),
	 g_id_to_external_ghost_box(g.g_id_to_external_ghost_box),
	 g_id_to_internal_ghost_box(g.g_id_to_internal_ghost_box),
	 ginfo(g.ginfo),
	 ginfo_v(g.ginfo_v),
	 init_local_i_g_box(g.init_local_i_g_box),
	 init_local_e_g_box(g.init_local_e_g_box)
	{
#ifdef SE_CLASS2
		check_new(this,8,GRID_DIST_EVENT,4);
#endif

		for (size_t i = 0 ; i < dim ; i++)
		{g_sz[i] = g.g_sz[i];}
	}

	/*! \brief This constructor is special, it construct an expanded grid that perfectly overlap with the previous
	 *
	 * The key-word here is "perfectly overlap". Using the default constructor you could create
	 * something similar, but because of rounding-off error it can happen that it is not perfectly overlapping
	 *
	 * \param g previous grid
	 * \param gh Ghost part in grid units
	 * \param ext extension of the grid (must be positive on every direction)
	 *
	 */
	template<typename H>
	grid_dist_id(const grid_dist_id<dim,St,H,typename Decomposition::base_type,Memory,grid_cpu<dim,H>> & g,
			     const Ghost<dim,long int> & gh,
				 Box<dim,size_t> ext)
	:ghost_int(gh),dec(create_vcluster()),v_cl(create_vcluster())
	{
#ifdef SE_CLASS2
		check_new(this,8,GRID_DIST_EVENT,4);
#endif

		size_t ext_dim[dim];
		for (size_t i = 0 ; i < dim ; i++) {ext_dim[i] = g.getGridInfoVoid().size(i) + ext.getKP1().get(i) + ext.getKP2().get(i);}

		// Set the grid info of the extended grid
		ginfo.setDimensions(ext_dim);
		ginfo_v.setDimensions(ext_dim);

		InitializeCellDecomposer(g.getCellDecomposer(),ext);

		ghost = convert_ghost(gh,cd_sm);

		// Extend the grid by the extension part and calculate the domain

		for (size_t i = 0 ; i < dim ; i++)
		{
			g_sz[i] = g.size(i) + ext.getLow(i) + ext.getHigh(i);

			if (g.getDecomposition().periodicity(i) == NON_PERIODIC)
			{
				this->domain.setLow(i,g.getDomain().getLow(i) - ext.getLow(i) * g.spacing(i) - g.spacing(i) / 2.0);
				this->domain.setHigh(i,g.getDomain().getHigh(i) + ext.getHigh(i) * g.spacing(i) + g.spacing(i) / 2.0);
			}
			else
			{
				this->domain.setLow(i,g.getDomain().getLow(i) - ext.getLow(i) * g.spacing(i));
				this->domain.setHigh(i,g.getDomain().getHigh(i) + ext.getHigh(i) * g.spacing(i));
			}
		}

		dec.setParameters(g.getDecomposition(),ghost,this->domain);

		// an empty
		openfpm::vector<Box<dim,long int>> empty;

		InitializeStructures(g.getGridInfoVoid().getSize(),empty,gh,false);
	}

    /*! It constructs a grid of a specified size, defined on a specified Box space, forcing to follow a specified decomposition and with a specified ghost size
     *
     * \param dec Decomposition
     * \param g_sz grid size on each dimension
     * \param ghost Ghost part
     *
     */
    grid_dist_id(const Decomposition & dec,
    		     const size_t (& g_sz)[dim],
				 const Ghost<dim,St> & ghost)
    :domain(dec.getDomain()),ghost(ghost),ghost_int(INVALID_GHOST),dec(dec),v_cl(create_vcluster()),
	 ginfo(g_sz),ginfo_v(g_sz)
	{
#ifdef SE_CLASS2
		check_new(this,8,GRID_DIST_EVENT,4);
#endif

		InitializeCellDecomposer(g_sz,dec.periodicity());

		this->dec = dec.duplicate(ghost);

		InitializeStructures(g_sz);
	}

    /*! It constructs a grid of a specified size, defined on a specified Box space, forcing to follow a specified decomposition and with a specified ghost size
     *
     * \param dec Decomposition
     * \param g_sz grid size on each dimension
     * \param ghost Ghost part
     *
     */
    grid_dist_id(Decomposition && dec, const size_t (& g_sz)[dim],
    		     const Ghost<dim,St> & ghost)
    :domain(dec.getDomain()),ghost(ghost),dec(dec),ginfo(g_sz),
	 ginfo_v(g_sz),v_cl(create_vcluster()),ghost_int(INVALID_GHOST)
	{
#ifdef SE_CLASS2
		check_new(this,8,GRID_DIST_EVENT,4);
#endif

		InitializeCellDecomposer(g_sz,dec.periodicity());

		this->dec = dec.duplicate(ghost);

		InitializeStructures(g_sz);
	}

    /*! It constructs a grid of a specified size, defined on a specified Box space, forcing to follow a specified decomposition, and having a specified ghost size
     *
     * \param dec Decomposition
     * \param g_sz grid size on each dimension
     * \param g Ghost part (given in grid units)
     *
     * \warning In very rare case the ghost part can be one point bigger than the one specified
     *
     */
	grid_dist_id(const Decomposition & dec, const size_t (& g_sz)[dim],
			     const Ghost<dim,long int> & g)
	:domain(dec.getDomain()),ghost_int(g),dec(create_vcluster()),v_cl(create_vcluster()),
	 ginfo(g_sz),ginfo_v(g_sz)
	{
#ifdef SE_CLASS2
		check_new(this,8,GRID_DIST_EVENT,4);
#endif

		InitializeCellDecomposer(g_sz,dec.periodicity());

		ghost = convert_ghost(g,cd_sm);
		this->dec = dec.duplicate(ghost);

		// an empty
		openfpm::vector<Box<dim,long int>> empty;

		// Initialize structures
		InitializeStructures(g_sz,empty,g,false);
	}

    /*! It construct a grid of a specified size, defined on a specified Box space, forcing to follow a specified decomposition, and having a specified ghost size
     *
     * \param dec Decomposition
     * \param g_sz grid size on each dimension
     * \param g Ghost part (given in grid units)
     *
     * \warning In very rare case the ghost part can be one point bigger than the one specified
     *
     */
	grid_dist_id(Decomposition && dec, const size_t (& g_sz)[dim],
			     const Ghost<dim,long int> & g)
	:domain(dec.getDomain()),dec(dec),v_cl(create_vcluster()),ginfo(g_sz),
	 ginfo_v(g_sz),ghost_int(g)
	{
#ifdef SE_CLASS2
		check_new(this,8,GRID_DIST_EVENT,4);
#endif
		InitializeCellDecomposer(g_sz,dec.periodicity());

		ghost = convert_ghost(g,cd_sm);
		this->dec = dec.duplicate(ghost);

		// an empty
		openfpm::vector<Box<dim,long int>> empty;

		// Initialize structures
		InitializeStructures(g_sz,empty,g,false);
	}

    /*! It construct a grid of a specified size, defined on a specified Box space, and having a specified ghost size
     *
     * \param g_sz grid size on each dimension
     * \param domain Box that contain the grid
     * \param g Ghost part (given in grid units)
     *
     * \warning In very rare case the ghost part can be one point bigger than the one specified
     *
     */
	grid_dist_id(const size_t (& g_sz)[dim],const Box<dim,St> & domain, const Ghost<dim,St> & g, size_t opt = 0)
	:grid_dist_id(g_sz,domain,g,create_non_periodic<dim>(),opt)
	{
	}

    /*! It construct a grid of a specified size, defined on a specified Box space, having a specified ghost size and periodicity
     *
     * \param g_sz grid size on each dimension
     * \param domain Box that contain the grid
     * \param g Ghost part of the domain (given in grid units)
     *
     * \warning In very rare case the ghost part can be one point bigger than the one specified
     *
     */
	grid_dist_id(const size_t (& g_sz)[dim],const Box<dim,St> & domain, const Ghost<dim,long int> & g, size_t opt = 0)
	:grid_dist_id(g_sz,domain,g,create_non_periodic<dim>(),opt)
	{
	}

    /*! It construct a grid of a specified size, defined on a specified Box space, having a specified ghost size, and specified periodicity
     *
     * \param g_sz grid size on each dimension
     * \param domain Box that contain the grid
     * \param g Ghost part (given in grid units)
     * \param p Boundary conditions
     *
     * \warning In very rare case the ghost part can be one point bigger than the one specified
     *
     */
	grid_dist_id(const size_t (& g_sz)[dim],const Box<dim,St> & domain, const Ghost<dim,St> & g, const periodicity<dim> & p, size_t opt = 0)
	:domain(domain),ghost(g),ghost_int(INVALID_GHOST),dec(create_vcluster()),v_cl(create_vcluster()),ginfo(g_sz),ginfo_v(g_sz)
	{
#ifdef SE_CLASS2
		check_new(this,8,GRID_DIST_EVENT,4);
#endif

		if (opt >> 32 != 0)
		{this->setDecompositionGranularity(opt >> 32);}

		InitializeCellDecomposer(g_sz,p.bc);
		InitializeDecomposition(g_sz, p.bc);
		InitializeStructures(g_sz);
	}


    /*! It construct a grid of a specified size, defined on a specified Box space, having a specified ghost size and periodicity
     *
     * \param g_sz grid size on each dimension
     * \param domain Box that contain the grid
     * \param g Ghost part of the domain (given in grid units)
     * \param p periodicity
     *
     * \warning In very rare case the ghost part can be one point bigger than the one specified
     *
     */
	grid_dist_id(const size_t (& g_sz)[dim],const Box<dim,St> & domain, const Ghost<dim,long int> & g, const periodicity<dim> & p, size_t opt = 0)
	:domain(domain),ghost_int(g),dec(create_vcluster()),v_cl(create_vcluster()),ginfo(g_sz),ginfo_v(g_sz)
	{
#ifdef SE_CLASS2
		check_new(this,8,GRID_DIST_EVENT,4);
#endif

		if (opt >> 32 != 0)
		{this->setDecompositionGranularity(opt >> 32);}

		InitializeCellDecomposer(g_sz,p.bc);

		ghost = convert_ghost(g,cd_sm);

		InitializeDecomposition(g_sz,p.bc);

		// an empty
		openfpm::vector<Box<dim,long int>> empty;

		// Initialize structures
		InitializeStructures(g_sz,empty,g,false);
	}

	/*! \brief It construct a grid on the full domain restricted
	 *         to the set of boxes specified
	 *
	 * In particular the grid is defined in the space equal to the
	 *  domain intersected the boxes defined by bx
	 *
	 * \param g_sz grid size on each dimension
	 * \param domain where the grid is constructed
	 * \param g ghost size
	 * \param p periodicity of the grid
	 * \param bx set of boxes where the grid is defined
	 *
	 *
	 */
	grid_dist_id(const size_t (& g_sz)[dim],
			     const Box<dim,St> & domain,
				 const Ghost<dim,long int> & g,
				 const periodicity<dim> & p,
				 openfpm::vector<Box<dim,long int>> & bx_def)
	:domain(domain),dec(create_vcluster()),v_cl(create_vcluster()),ginfo(g_sz),ginfo_v(g_sz),gint(g)
	{
#ifdef SE_CLASS2
		check_new(this,8,GRID_DIST_EVENT,4);
#endif

		InitializeCellDecomposer(g_sz,p.bc);

		ghost = convert_ghost(g,cd_sm);

		InitializeDecomposition(g_sz, p.bc);
		InitializeStructures(g_sz,bx_def,g,true);
		this->bx_def = bx_def;
		this->use_bx_def = true;
	}

	/*! \brief Get an object containing the grid informations
	 *
	 * \return an information object about this grid
	 *
	 */
	const grid_sm<dim,T> & getGridInfo() const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return ginfo;
	}

	/*! \brief Get an object containing the grid informations without type
	 *
	 * \return an information object about this grid
	 *
	 */
	const grid_sm<dim,void> & getGridInfoVoid() const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return ginfo_v;
	}

	/*! \brief Get the object that store the information about the decomposition
	 *
	 * \return the decomposition object
	 *
	 */
	Decomposition & getDecomposition()
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return dec;
	}

	/*! \brief Get the object that store the information about the decomposition
	 *
	 * \return the decomposition object
	 *
	 */
	const Decomposition & getDecomposition() const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return dec;
	}

	/*! \brief Return the cell decomposer
	 *
	 * \return the cell decomposer
	 *
	 */
	const CellDecomposer_sm<dim,St,shift<dim,St>> & getCellDecomposer() const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return cd_sm;
	}

	/*! \brief Check that the global grid key is inside the grid domain
	 *
	 * \param gk point to check
	 *
	 * \return true if is inside
	 *
	 */
	bool isInside(const grid_key_dx<dim> & gk) const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		for (size_t i = 0 ; i < dim ; i++)
		{
			if (gk.get(i) < 0 || gk.get(i) >= (long int)g_sz[i])
			{return false;}
		}

		return true;
	}

	/*! \brief Get the total number of grid points for the calling processor
	 *
	 * \return The number of grid points
	 *
	 */
	size_t getLocalDomainSize() const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		size_t total = 0;

		for (size_t i = 0 ; i < gdb_ext.size() ; i++)
		{
			total += gdb_ext.get(i).Dbox.getVolumeKey();
		}

		return total;
	}

	/*! \brief Get the total number of grid points with ghost for the calling processor
	 *
	 * \return The number of grid points
	 *
	 */
	size_t getLocalDomainWithGhostSize() const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		size_t total = 0;

		for (size_t i = 0 ; i < gdb_ext.size() ; i++)
		{
			total += gdb_ext.get(i).GDbox.getVolumeKey();
		}

		return total;
	}


	/*! \brief It return the informations about the local grids
	 *
	 * \return The information about the local grids
	 *
	 */
	const openfpm::vector<GBoxes<device_grid::dims>> & getLocalGridsInfo()
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return gdb_ext;
	}

	/*! \brief It gathers the information about local grids for all of the processors
	 *
	 * \param gdb_ext_global where to store the grid infos
	 *
	 */
	void getGlobalGridsInfo(openfpm::vector<GBoxes<device_grid::dims>> & gdb_ext_global) const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		gdb_ext_global.clear();

		v_cl.SGather(gdb_ext,gdb_ext_global,0);
		v_cl.execute();

		size_t size_r;
		size_t size = gdb_ext_global.size();

		if (v_cl.getProcessUnitID()  == 0)
		{
			for (size_t i = 1; i < v_cl.getProcessingUnits(); i++)
				v_cl.send(i,0,&size,sizeof(size_t));

			size_r = size;
		}
		else
			v_cl.recv(0,0,&size_r,sizeof(size_t));

		v_cl.execute();

		gdb_ext_global.resize(size_r);


		if (v_cl.getProcessUnitID()  == 0)
		{
			for (size_t i = 1; i < v_cl.getProcessingUnits(); i++)
				v_cl.send(i,0,gdb_ext_global);
		}
		else
			v_cl.recv(0,0,gdb_ext_global);

		v_cl.execute();
	}


	/*! \brief It return an iterator that span the full grid domain (each processor span its local domain)
	 *
	 * \return the iterator
	 *
	 */
	grid_dist_iterator<dim,device_grid,
					   decltype(device_grid::type_of_subiterator()),
					   FREE>
	getOldDomainIterator() const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif

		grid_key_dx<dim> stop(ginfo_v.getSize());
		grid_key_dx<dim> one;
		one.one();
		stop = stop - one;

		grid_dist_iterator<dim,device_grid,
							decltype(device_grid::type_of_subiterator()),
							FREE> it(loc_grid_old,gdb_ext_old,stop);

		return it;
	}

	/*! /brief Get a grid Iterator
	 *
	 * In case of dense grid getGridIterator is equivalent to getDomainIterator
	 * in case of sparse grid getDomainIterator go across all the
	 * inserted point get grid iterator run across all grid points independently
	 * that the point has been insert or not
	 *
	 * \return a Grid iterator
	 *
	 */
	inline grid_dist_id_iterator_dec<Decomposition> getGridIterator(const grid_key_dx<dim> & start, const grid_key_dx<dim> & stop)
	{
		grid_dist_id_iterator_dec<Decomposition> it_dec(getDecomposition(), g_sz, start, stop);
		return it_dec;
	}

#ifdef __NVCC__

	/*! /brief Get a grid Iterator in GPU
	 *
	 * In case of dense grid getGridIterator is equivalent to getDomainIteratorGPU
	 * in case of sparse distributed grid getDomainIterator go across all the
	 * inserted point getGridIteratorGPU run across all grid points independently
	 * that the point has been insert or not
	 *
	 * \param start point
	 * \param stop point
	 *
	 * \return a Grid iterator
	 *
	 */
	inline grid_dist_id_iterator_gpu<Decomposition,openfpm::vector<device_grid>>
	getGridIteratorGPU(const grid_key_dx<dim> & start, const grid_key_dx<dim> & stop)
	{
		grid_dist_id_iterator_gpu<Decomposition,openfpm::vector<device_grid>> it_dec(loc_grid,getDecomposition(), g_sz, start, stop);
		return it_dec;
	}

	/*! /brief Get a grid Iterator in GPU
	 *
	 * In case of dense grid getGridIterator is equivalent to getDomainIteratorGPU
	 * in case of sparse distributed grid getDomainIterator go across all the
	 * inserted point getGridIteratorGPU run across all grid points independently
	 * that the point has been insert or not
	 *
	 * \return a Grid iterator
	 *
	 */
	inline grid_dist_id_iterator_gpu<Decomposition,openfpm::vector<device_grid>>
	getGridIteratorGPU()
	{
		grid_dist_id_iterator_gpu<Decomposition,openfpm::vector<device_grid>> it_dec(loc_grid,getDecomposition(), g_sz);
		return it_dec;
	}

#endif

	/*! /brief Get a grid Iterator running also on ghost area
	 *
	 * In case of dense grid getGridIterator is equivalent to getDomainIterator
	 * in case if sparse distributed grid getDomainIterator go across all the
	 * inserted point get grid iterator run across all grid points independently
	 * that the point has been insert or not
	 *
	 * \return a Grid iterator
	 *
	 */
	inline grid_dist_id_iterator_dec<Decomposition,true> getGridGhostIterator(const grid_key_dx<dim> & start, const grid_key_dx<dim> & stop)
	{
		grid_dist_id_iterator_dec<Decomposition,true> it_dec(getDecomposition(), g_sz, start, stop);
		return it_dec;
	}

	/*! /brief Get a grid Iterator
	 *
	 * In case of dense grid getGridIterator is equivalent to getDomainIterator
	 * in case if sparse distributed grid getDomainIterator go across all the
	 * inserted point get grid iterator run across all grid points independently
	 * that the point has been insert or not
	 *
	 * \return a Grid iterator
	 *
	 */
	inline grid_dist_id_iterator_dec<Decomposition> getGridIterator()
	{
		grid_key_dx<dim> start;
		grid_key_dx<dim> stop;
		for (size_t i = 0; i < dim; i++)
		{
			start.set_d(i, 0);
			stop.set_d(i, g_sz[i] - 1);
		}

		grid_dist_id_iterator_dec<Decomposition> it_dec(getDecomposition(), g_sz, start, stop);
		return it_dec;
	}

	/*! \brief It return an iterator that span the full grid domain (each processor span its local domain)
	 *
	 * \return the iterator
	 *
	 */
	grid_dist_iterator<dim,device_grid,
					   decltype(device_grid::type_of_subiterator()),FREE>
	getDomainIterator() const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif

		grid_key_dx<dim> stop(ginfo_v.getSize());
		grid_key_dx<dim> one;
		one.one();
		stop = stop - one;

		grid_dist_iterator<dim,device_grid,
							decltype(device_grid::type_of_subiterator()),
							FREE> it(loc_grid,gdb_ext,stop);

		return it;
	}

	/*! \brief It return an iterator that span the full grid domain (each processor span its local domain)
	 *
	 * \param stencil_pnt stencil points
	 *
	 * \return the iterator
	 *
	 */
	template<unsigned int Np>
	grid_dist_iterator<dim,device_grid,
						decltype(device_grid::template type_of_subiterator<stencil_offset_compute<dim,Np>>()),
						FREE,
						stencil_offset_compute<dim,Np> >
	getDomainIteratorStencil(const grid_key_dx<dim> (& stencil_pnt)[Np]) const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif

		grid_key_dx<dim> stop(ginfo_v.getSize());
		grid_key_dx<dim> one;
		one.one();
		stop = stop - one;

		grid_dist_iterator<dim,device_grid,
						   decltype(device_grid::template type_of_subiterator<stencil_offset_compute<dim,Np>>()),
						   FREE,
						   stencil_offset_compute<dim,Np>> it(loc_grid,gdb_ext,stop,stencil_pnt);

		return it;
	}

	/*! \brief It return an iterator that span the grid domain + ghost part
	 *
	 * \return the iterator
	 *
	 */
	grid_dist_iterator<dim,device_grid,
	decltype(device_grid::type_of_iterator()),
	FIXED>
	getDomainGhostIterator() const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		grid_key_dx<dim> stop;
		for (size_t i = 0 ; i < dim ; i++)
		{stop.set_d(i,0);}

		grid_dist_iterator<dim,device_grid,
							decltype(device_grid::type_of_iterator()),
							FIXED> it(loc_grid,gdb_ext,stop);

		return it;
	}

	/*! \brief It return an iterator that span the grid domain only in the specified
	 * part
	 *
	 * The key spanned are the one inside the box spanned by the start point and the end
	 * point included
	 *
	 * \param start point
	 * \param stop point
	 *
	 * \return the sub-domain iterator
	 *
	 */
	grid_dist_iterator_sub<dim,device_grid>
	getSubDomainIterator(const grid_key_dx<dim> & start,
						 const grid_key_dx<dim> & stop) const
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		grid_dist_iterator_sub<dim,device_grid> it(start,stop,loc_grid,gdb_ext);

		return it;
	}

	/*! \brief It return an iterator that span the grid domain only in the specified
	 * part
	 *
	 * The key spanned are the one inside the box spanned by the start point and the end
	 * point included
	 *
	 * \param start point
	 * \param stop point
	 *
	 * \return an iterator on the sub-part of the grid
	 *
	 */
	grid_dist_iterator_sub<dim,device_grid> getSubDomainIterator(const long int (& start)[dim], const long int (& stop)[dim]) const
	{
		grid_dist_iterator_sub<dim,device_grid> it(grid_key_dx<dim>(start),grid_key_dx<dim>(stop),loc_grid,gdb_ext);

		return it;
	}

	//! Destructor
	~grid_dist_id()
	{
#ifdef SE_CLASS2
		check_delete(this);
#endif
		dec.decRef();
	}

	/*! \brief Get the Virtual Cluster machine
	 *
	 * \return the Virtual cluster machine
	 *
	 */
	Vcluster<> & getVC()
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return v_cl;
	}

	/*! \brief Indicate that this grid is not staggered
	 *
	 * \return false
	 *
	 */
	bool is_staggered()
	{
		return false;
	}

	/*! \brief remove an element in the grid
	 *
	 * In case of dense grid this function print a warning, in case of sparse
	 * grid this function remove a grid point.
	 *
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return a reference to the inserted element
	 *
	 */
	template <typename bg_key> inline void remove(const grid_dist_key_dx<dim,bg_key> & v1)
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return loc_grid.get(v1.getSub()).remove(v1.getKey());
	}

	/*! \brief remove an element in the grid
	 *
	 * In case of dense grid this function print a warning, in case of sparse
	 * grid this function remove a grid point.
	 *
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return a reference to the inserted element
	 *
	 */
	template <typename bg_key> inline void remove_no_flush(const grid_dist_key_dx<dim,bg_key> & v1)
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return loc_grid.get(v1.getSub()).remove_no_flush(v1.getKey());
	}


	template<typename ... v_reduce>
	void flush(flush_type opt = flush_type::FLUSH_ON_HOST)
	{
		for (size_t i = 0 ; i < loc_grid.size() ; i++)
		{
			loc_grid.get(i).template flush<v_reduce ...>(v_cl.getmgpuContext(),opt);
		}
	}

	/*! \brief remove an element in the grid
	 *
	 * In case of dense grid this function print a warning, in case of sparse
	 * grid this function remove a grid point.
	 *
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return a reference to the inserted element
	 *
	 */
	inline void flush_remove()
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		for (size_t i = 0 ; i < loc_grid.size() ; i++)
		{loc_grid.get(i).flush_remove();}
	}

	/*! \brief insert an element in the grid
	 *
	 * In case of dense grid this function is equivalent to get, in case of sparse
	 * grid this function insert a grid point. When the point already exist it return
	 * a reference to the already existing point
	 *
	 * \tparam p property to get (is an integer)
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return a reference to the inserted element
	 *
	 */
	template <unsigned int p,typename bg_key>inline auto insert(const grid_dist_key_dx<dim,bg_key> & v1)
	-> typename std::add_lvalue_reference
	<
		decltype(loc_grid.get(v1.getSub()).template insert<p>(v1.getKey()))
	>::type
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif

		return loc_grid.get(v1.getSub()).template insert<p>(v1.getKey());
	}


	/*! \brief Get the reference of the selected element
	 *
	 * \tparam p property to get (is an integer)
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return the selected element
	 *
	 */
	template <unsigned int p, typename bg_key>
	inline auto get(const grid_dist_key_dx<dim,bg_key> & v1) const
	-> typename std::add_lvalue_reference<decltype(loc_grid.get(v1.getSub()).template get<p>(v1.getKey()))>::type
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return loc_grid.get(v1.getSub()).template get<p>(v1.getKey());
	}


	/*! \brief Get the reference of the selected element
	 *
	 * \tparam p property to get (is an integer)
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return the selected element
	 *
	 */
	template <unsigned int p, typename bg_key>
	inline auto get(const grid_dist_key_dx<dim,bg_key> & v1)
	-> typename std::add_lvalue_reference<decltype(loc_grid.get(v1.getSub()).template get<p>(v1.getKey()))>::type
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return loc_grid.get(v1.getSub()).template get<p>(v1.getKey());
	}

	/*! \brief Get the reference of the selected element
	 *
	 * \tparam p property to get (is an integer)
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return the selected element
	 *
	 */
	template <unsigned int p = 0>
	inline auto get(const grid_dist_g_dx<device_grid> & v1) const -> typename std::add_lvalue_reference<decltype(v1.getSub()->template get<p>(v1.getKey()))>::type
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return v1.getSub()->template get<p>(v1.getKey());
	}

	/*! \brief Get the reference of the selected element
	 *
	 * \tparam p property to get (is an integer)
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return the selected element
	 *
	 */
	template <unsigned int p = 0>
	inline auto get(const grid_dist_g_dx<device_grid> & v1) -> typename std::add_lvalue_reference<decltype(v1.getSub()->template get<p>(v1.getKey()))>::type
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return v1.getSub()->template get<p>(v1.getKey());
	}

	/*! \brief Get the reference of the selected element
	 *
	 * \tparam p property to get (is an integer)
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return the selected element
	 *
	 */
	template <unsigned int p = 0>
	inline auto get(const grid_dist_lin_dx & v1) const -> typename std::add_lvalue_reference<decltype(loc_grid.get(v1.getSub()).template get<p>(v1.getKey()))>::type
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return loc_grid.get(v1.getSub()).template get<p>(v1.getKey());
	}

	/*! \brief Get the reference of the selected element
	 *
	 * \tparam p property to get (is an integer)
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return the selected element
	 *
	 */
	template <unsigned int p = 0>
	inline auto get(const grid_dist_lin_dx & v1) -> typename std::add_lvalue_reference<decltype(loc_grid.get(v1.getSub()).template get<p>(v1.getKey()))>::type
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		return loc_grid.get(v1.getSub()).template get<p>(v1.getKey());
	}

	/*! \brief Get the reference of the selected element
	 *
	 * \tparam p property to get (is an integer)
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return the selected element
	 *
	 */
	template <unsigned int p = 0>
	inline auto getProp(const grid_dist_key_dx<dim> & v1) const -> decltype(this->template get<p>(v1))
	{
		return this->template get<p>(v1);
	}

	/*! \brief Get the reference of the selected element
	 *
	 * \tparam p property to get (is an integer)
	 * \param v1 grid_key that identify the element in the grid
	 *
	 * \return the selected element
	 *
	 */
	template <unsigned int p = 0>
	inline auto getProp(const grid_dist_key_dx<dim> & v1) -> decltype(this->template get<p>(v1))
	{
		return this->template get<p>(v1);
	}

	/*! \brief It synchronize the ghost parts
	 *
	 * \tparam prp... Properties to synchronize
	 *
	 */
	template<int... prp> void ghost_get()
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif

		// Convert the ghost  internal boxes into grid unit boxes
		create_ig_box();

		// Convert the ghost external boxes into grid unit boxes
		create_eg_box();

		// Convert the local ghost internal boxes into grid unit boxes
		create_local_ig_box();

		// Convert the local external ghost boxes into grid unit boxes
		create_local_eg_box();

		grid_dist_id_comm<dim,St,T,Decomposition,Memory,device_grid>::template ghost_get_<prp...>(ig_box,
																								  eg_box,
																								  loc_ig_box,
																								  loc_eg_box,
																								  gdb_ext,
																								  eb_gid_list,
																								  use_bx_def,
																								  loc_grid,
																								  ginfo_v,
																								  g_id_to_external_ghost_box);
	}

	/*! \brief It synchronize the ghost parts
	 *
	 * \tparam prp... Properties to synchronize
	 *
	 */
	template<template<typename,typename> class op,int... prp> void ghost_put()
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif

		// Convert the ghost  internal boxes into grid unit boxes
		create_ig_box();

		// Convert the ghost external boxes into grid unit boxes
		create_eg_box();

		// Convert the local ghost internal boxes into grid unit boxes
		create_local_ig_box();

		// Convert the local external ghost boxes into grid unit boxes
		create_local_eg_box();

		grid_dist_id_comm<dim,St,T,Decomposition,Memory,device_grid>::template ghost_put_<op,prp...>(dec,
																									 ig_box,
																									 eg_box,
																									 loc_ig_box,
																									 loc_eg_box,
																									 gdb_ext,
																									 loc_grid,
																						  	  	  	 g_id_to_internal_ghost_box);
	}


	/*! \brief Copy the give grid into this grid
	 *
	 * It copy the first grid into the given grid (No ghost)
	 *
	 * \warning the Decomposition must be ensured to be the same, otherwise crashes can happen, if you want to copy the grid independently from the decomposition please use the operator equal
	 *
	 * \param g Grid to copy
	 * \param use_memcpy use memcpy function if possible
	 *
	 * \return itself
	 *
	 */
	grid_dist_id<dim,St,T,Decomposition,Memory,device_grid> & copy(grid_dist_id<dim,St,T,Decomposition,Memory,device_grid> & g, bool use_memcpy = true)
	{
		if (T::noPointers() == true && use_memcpy)
		{
			for (size_t i = 0 ; i < this->getN_loc_grid() ; i++)
			{
				auto & gs_src = this->get_loc_grid(i).getGrid();

				long int start = gs_src.LinId(gdb_ext.get(i).Dbox.getKP1());
				long int stop = gs_src.LinId(gdb_ext.get(i).Dbox.getKP2());

				if (stop < start) {continue;}

				void * dst = static_cast<void *>(static_cast<char *>(this->get_loc_grid(i).getPointer()) + start*sizeof(T));
				void * src = static_cast<void *>(static_cast<char *>(g.get_loc_grid(i).getPointer()) + start*sizeof(T));

				memcpy(dst,src,sizeof(T) * (stop + 1 - start));
			}
		}
		else
		{
			grid_key_dx<dim> cnt[1];
			cnt[0].zero();

			for (size_t i = 0 ; i < this->getN_loc_grid() ; i++)
			{
				auto & dst = this->get_loc_grid(i);
				auto & src = g.get_loc_grid(i);

				auto it = this->get_loc_grid_iterator_stencil(i,cnt);

				while (it.isNext())
				{
					// center point
					auto Cp = it.template getStencil<0>();

					dst.get_o(Cp) = src.get_o(Cp);

					++it;
				}
			}
		}

		return *this;
	}

	/*! \brief Get the spacing on each dimension
	 *
	 * \return the spacing of the grid on each dimension as a point
	 *
	 */
	Point<dim,St> getSpacing()
	{
		return cd_sm.getCellBox().getP2();
	}

	/*! \brief Convert a g_dist_key_dx into a global key
	 *
	 * \see grid_dist_key_dx
	 * \see grid_dist_iterator
	 *
	 * \param k grid_dist_key_dx point (in general returned by the iterators)
	 *
	 * \return the global position in the grid
	 *
	 */
	inline grid_key_dx<dim> getGKey(const grid_dist_key_dx<dim> & k)
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		// Get the sub-domain id
		size_t sub_id = k.getSub();

		grid_key_dx<dim> k_glob = k.getKey();

		// shift
		k_glob = k_glob + gdb_ext.get(sub_id).origin;

		return k_glob;
	}

	/*! \brief Add the computation cost on the decomposition using a resolution function
	 *
	 *
	 * \param md Model to use
	 * \param ts It is an optional parameter approximately should be the number of ghost get between two
	 *           rebalancing at first decomposition this number can be ignored (default = 1) because not used
	 *
	 */
	template <typename Model>inline void addComputationCosts(Model md=Model(), size_t ts = 1)
	{
		CellDecomposer_sm<dim, St, shift<dim,St>> cdsm;

		Decomposition & dec = getDecomposition();
		auto & dist = getDecomposition().getDistribution();

		cdsm.setDimensions(dec.getDomain(), dec.getDistGrid().getSize(), 0);

		// Invert the id to positional

		Point<dim,St> p;
		for (size_t i = 0; i < dist.getNOwnerSubSubDomains() ; i++)
		{
			dist.getSubSubDomainPos(i,p);
			dec.setSubSubDomainComputationCost(dist.getOwnerSubSubDomain(i) , 1 + md.resolution(p));
		}

		dec.computeCommunicationAndMigrationCosts(ts);

		dist.setDistTol(md.distributionTol());
	}

	/*! \brief Write the distributed grid information
	 *
	 * * grid_X.vtk Output each local grids for each local processor X
	 * * internal_ghost_X.vtk Internal ghost boxes in grid units for the local processor X
	 *
	 * \param output directory where to put the files + prefix
	 * \param opt options
	 *
	 * \return true if the write operation succeed
	 *
	 */
	bool write(std::string output, size_t opt = VTK_WRITER | FORMAT_ASCII )
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif

		file_type ft = file_type::ASCII;

		if (opt & FORMAT_BINARY)
			ft = file_type::BINARY;

		// Create a writer and write
		VTKWriter<boost::mpl::pair<device_grid,float>,VECTOR_GRIDS> vtk_g;
		for (size_t i = 0 ; i < loc_grid.size() ; i++)
		{
			Point<dim,St> offset = getOffset(i);

			if (opt & PRINT_GHOST)
			{vtk_g.add(loc_grid.get(i),offset,cd_sm.getCellBox().getP2(),gdb_ext.get(i).GDbox);}
			else
			{vtk_g.add(loc_grid.get(i),offset,cd_sm.getCellBox().getP2(),gdb_ext.get(i).Dbox);}
		}
		vtk_g.write(output + "_" + std::to_string(v_cl.getProcessUnitID()) + ".vtk", prp_names, "grids", ft);

		return true;
	}

	/*! \brief Write the distributed grid information
	 *
	 * * grid_X.vtk Output each local grids for each local processor X
	 * * internal_ghost_X.vtk Internal ghost boxes in grid units for the local processor X
	 *
	 * \param output directory where to put the files + prefix
	 * \param i frame number
	 * \param opt options
	 *
	 * \return true id the write succeed
	 *
	 */
	bool write_frame(std::string output, size_t i, size_t opt = VTK_WRITER | FORMAT_ASCII)
	{
#ifdef SE_CLASS2
		check_valid(this,8);
#endif
		file_type ft = file_type::ASCII;

		if (opt & FORMAT_BINARY)
			ft = file_type::BINARY;

		// Create a writer and write
		VTKWriter<boost::mpl::pair<device_grid,float>,VECTOR_GRIDS> vtk_g;
		for (size_t i = 0 ; i < loc_grid.size() ; i++)
		{
			Point<dim,St> offset = getOffset(i);
			vtk_g.add(loc_grid.get(i),offset,cd_sm.getCellBox().getP2(),gdb_ext.get(i).Dbox);
		}
		vtk_g.write(output + "_" + std::to_string(v_cl.getProcessUnitID()) + "_" + std::to_string(i) + ".vtk",prp_names,"grids",ft);

		return true;
	}



	/*! \brief Get the i sub-domain grid
	 *
	 * \param i sub-domain
	 *
	 * \return local grid
	 *
	 */
	device_grid & get_loc_grid(size_t i)
	{
		return loc_grid.get(i);
	}

	/*! \brief Get the i sub-domain grid
	 *
	 * \param i sub-domain
	 *
	 * \return local grid
	 *
	 */
	grid_key_dx_iterator_sub<dim,no_stencil> get_loc_grid_iterator(size_t i)
	{
		return grid_key_dx_iterator_sub<dim,no_stencil>(loc_grid.get(i).getGrid(),
				 gdb_ext.get(i).Dbox.getKP1(),
				 gdb_ext.get(i).Dbox.getKP2());
	}

	/*! \brief Get the i sub-domain grid
	 *
	 * \param i sub-domain
	 *
	 * \return local grid
	 *
	 */
	template<unsigned int Np>
	grid_key_dx_iterator_sub<dim,stencil_offset_compute<dim,Np>> get_loc_grid_iterator_stencil(size_t i,const grid_key_dx<dim> (& stencil_pnt)[Np])
	{
		return grid_key_dx_iterator_sub<dim,stencil_offset_compute<dim,Np>>(loc_grid.get(i).getGrid(),
													 gdb_ext.get(i).Dbox.getKP1(),
													 gdb_ext.get(i).Dbox.getKP2(),
													 stencil_pnt);
	}

	/*! \brief Return the number of local grid
	 *
	 * \return the number of local grid
	 *
	 */
	size_t getN_loc_grid()
	{
		return loc_grid.size();
	}


	/*! \brief It return the id of structure in the allocation list
	 *
	 * \see print_alloc and SE_CLASS2
	 *
	 * \return the id
	 *
	 */
	long int who()
	{
#ifdef SE_CLASS2
		return check_whoami(this,8);
#else
			return -1;
#endif
	}

	/*! \brief It print the internal ghost boxes and external ghost boxes in global unit
	 *
	 *
	 */
	void debugPrint()
	{
		size_t tot_volume = 0;

		std::cout << "-------- External Ghost boxes ---------- " << std::endl;

		for (size_t i = 0 ; i < eg_box.size() ; i++)
		{
			std::cout << "Processor: " << eg_box.get(i).prc << " Boxes:" << std::endl;

			for (size_t j = 0; j < eg_box.get(i).bid.size() ; j++)
			{
				std::cout << " Box: " << eg_box.get(i).bid.get(j).g_e_box.toString() << "   Id: " << eg_box.get(i).bid.get(j).g_id << std::endl;
				tot_volume += eg_box.get(i).bid.get(j).g_e_box.getVolumeKey();
			}
		}

		std::cout << "TOT volume external ghost " << tot_volume << std::endl;

		std::cout << "-------- Internal Ghost boxes ---------- " << std::endl;

		tot_volume = 0;
		for (size_t i = 0 ; i < ig_box.size() ; i++)
		{
			std::cout << "Processor: " << ig_box.get(i).prc << " Boxes:" << std::endl;

			for (size_t j = 0 ; j < ig_box.get(i).bid.size() ; j++)
			{
				std::cout << " Box: " << ig_box.get(i).bid.get(j).box.toString() << "   Id: " << ig_box.get(i).bid.get(j).g_id << std::endl;
				tot_volume += ig_box.get(i).bid.get(j).box.getVolumeKey();
			}
		}

		std::cout << "TOT volume internal ghost " << tot_volume << std::endl;
	}

	/*! \brief Set the properties names
	 *
	 * It is useful to specify name for the properties in vtk writers
	 *
	 * \param names set of properties names
	 *
	 */
	void setPropNames(const openfpm::vector<std::string> & names)
	{
		prp_names = names;
	}

	/*! \brief It delete all the points
	 *
	 * This function on dense does nothing in case of dense grid but in case of
	 * sparse_grid it kills all the points
	 *
	 */
	void clear()
	{
		for (size_t i = 0 ; i < loc_grid.size() ; i++)
		{loc_grid.get(i).clear();}
	}

	/*! \brief It move all the grid parts that do not belong to the local processor to the respective processor
	 *
	 */
	void map(size_t opt = 0)
	{
		// Save the background values
		T bv;
		meta_copy<T>::meta_copy_(bv,loc_grid.get(0).getBackgroundValue());

		if (!(opt & NO_GDB_EXT_SWITCH))
		{
			gdb_ext_old = gdb_ext;
			loc_grid_old = loc_grid;

			InitializeStructures(g_sz,bx_def,gint,bx_def.size() != 0);
		}

		getGlobalGridsInfo(gdb_ext_global);

		this->map_(dec,cd_sm,loc_grid,loc_grid_old,gdb_ext,gdb_ext_old,gdb_ext_global);

		loc_grid_old.clear();
		loc_grid_old.shrink_to_fit();
		gdb_ext_old.clear();

		// reset ghost structure to recalculate
		reset_ghost_structures();

		// Reset the background values
		setBackgroundValue(bv);
	}

	/*! \brief Save the grid state on HDF5
	 *
	 * \param filename output filename
	 *
	 */
	inline void save(const std::string & filename) const
	{
		HDF5_writer<GRID_DIST> h5s;

		h5s.save(filename,loc_grid,gdb_ext);
	}

	/*! \brief Reload the grid from HDF5 file
	 *
	 * \param filename output filename
	 *
	 */
	inline void load(const std::string & filename)
	{
		HDF5_reader<GRID_DIST> h5l;

		h5l.load<device_grid>(filename,loc_grid_old,gdb_ext_old);

		// Map the distributed grid
		map(NO_GDB_EXT_SWITCH);
	}

	/*! \brief This is a meta-function return which type of sub iterator a grid produce
	 *
	 * \return the type of the sub-grid iterator
	 *
	 */
	template <typename stencil = no_stencil>
	static grid_dist_iterator_sub<dim,device_grid> type_of_subiterator()
	{
		return grid_key_dx_iterator_sub<dim, stencil>();
	}

	/*! \brief Get the internal local ghost box
	 *
	 * \return the internal local ghost box
	 *
	 */
	const openfpm::vector<i_lbox_grid<dim>> & get_loc_ig_box()
	{
		return this->loc_ig_box;
	}

	/*! \brief Get the internal ghost box
	 *
	 * \return the internal local ghost box
	 *
	 */
	const openfpm::vector<i_lbox_grid<dim>> & get_ig_box()
	{
		return this->ig_box;
	}

#ifdef __NVCC__

	/*! \brief Set the size of the gpu insert buffer pool
	 *
	 * Indicate the maximum number of inserts each GPU block can do
	 *
	 * \param size of the insert pool
	 *
	 */
	void setInsertBuffer(size_t n_pool)
	{
		gpu_insert_pool_size = n_pool;
	}

	template<typename func_t,typename it_t, typename ... args_t>
	void iterateGridGPU(it_t & it, args_t ... args)
	{
		while(it.isNextGrid())
		{
			Box<dim,size_t> b = it.getGridBox();

			size_t i = it.getGridId();

			auto ite = loc_grid.get(i).getGridGPUIterator(b.getKP1int(),b.getKP2int());

			loc_grid.get(i).setGPUInsertBuffer(ite.nblocks(),gpu_insert_pool_size);
			loc_grid.get(i).initializeGPUInsertBuffer();

			ite_gpu_dist<dim> itd = ite;

			for (int j = 0 ; j < dim ; j++)
			{
				itd.origin.set_d(j,gdb_ext.get(i).origin.get(j));
				itd.start_base.set_d(j,0);
			}

			grid_apply_functor<<<ite.wthr,ite.thr>>>(loc_grid.get(i).toKernel(),itd,func_t(),args...);

			it.nextGrid();
		}
	}

	template<typename func_t, typename ... args_t>
	void iterateGPU(args_t ... args)
	{
		for (int i = 0 ; i < loc_grid.size() ; i++)
		{
			auto & sp = loc_grid.get(i);

			// TODO Launch a kernel on every sparse grid GPU
		}
	}

	/*! \brief Move the memory from the device to host memory
	 *
	 */
	template<unsigned int ... prp> void deviceToHost()
	{
		for (size_t i = 0 ; i < loc_grid.size() ; i++)
		{
			loc_grid.get(i).template deviceToHost<prp ...>();
		}
	}

	/*! \brief Move the memory from the device to host memory
	 *
	 */
	template<unsigned int ... prp> void hostToDevice()
	{
		for (size_t i = 0 ; i < loc_grid.size() ; i++)
		{
			loc_grid.get(i).template hostToDevice<prp ...>();
		}
	}

#endif


	//! Define friend classes
	//\cond
	friend grid_dist_id<dim,St,T,typename Decomposition::extended_type,Memory,device_grid>;
	//\endcond
};


template<unsigned int dim, typename St, typename T>
using sgrid_dist_id = grid_dist_id<dim,St,T,CartDecomposition<dim,St>,HeapMemory,sgrid_cpu<dim,T,HeapMemory>>;

#ifdef __NVCC__
template<unsigned int dim, typename St, typename T>
using sgrid_dist_id_gpu = grid_dist_id<dim,St,T,CartDecomposition<dim,St,CudaMemory,memory_traits_inte>,CudaMemory,SparseGridGpu<dim,T>>;
#endif

#endif
