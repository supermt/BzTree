#include <thread>

#include "bztree.h"
#include "bzerrno.h"

struct bz_node_fake{};
POBJ_LAYOUT_BEGIN(layout_name);
POBJ_LAYOUT_TOID(layout_name, struct bz_node_fake);
POBJ_LAYOUT_END(layout_name);

/* ����key�Ĳ���λ�ã����ʱȡ����λ�ã� */
/* �ն�����£�����̽���ҵ��ǿն�Ԫ�� */
template<typename Key, typename Val>
int bz_node<Key, Val>::binary_search(uint64_t * meta_arr, int size, const Key * key)
{
	int left = -1, right = size;
	while (left + 1 < right) {
		int mid = left + (right - left) / 2;

		/* �ն����� BEGIN */
		if (!is_visiable(meta_arr[mid])) {
			bool go_left = true;
			int left_mid = mid - 1, right_mid = mid + 1;
			while (left_mid > left || right_mid < right) {
				if (go_left && left_mid > left || right_mid == right) {
					if (is_visiable(meta_arr[left_mid])) {
						mid = left_mid;
						break;
					}
					go_left = false;
					--left_mid;
				}
				if (!go_left && right_mid < right || left_mid == left) {
					if (is_visiable(meta_arr[right_mid])) {
						mid = right_mid;
						break;
					}
					go_left = true;
					++right_mid;
				}
			}
			if (left_mid == left && right_mid == right) {
				return right;
			}
		}
		/* �ն����� END */

		if (key_cmp(meta_arr[mid], key) < 0)
			left = mid;
		else
			right = mid;
	}
	return right;
}

/*
insert:
1) writer tests Frozen Bit, retraverse if failed
to avoid duplicate keys:
2) scan the sorted keys, fail if found a duplicate key
3) scan the unsorted keys
3.2) fail if meet a duplicate key
3.1) if meet a meta entry whose visiable unset and epoch = current global epoch,
set Retry bit and continue
4) reserve space in meta entry and block
by adding one to record count and adding data length to node block size(both in node status)
and flipping meta entry offset's high bit and setting the rest bits to current index global epoch
4.1) in case fail => concurrent thread may be trying to insert duplicate key,
so need to set Recheck flag
5) unset visiable bit and copy data to block
6) persist
6.5) re-scan prior positions if Recheck is set
6.5.1) if find a duplicate key, set offset and block = 0
return fail
7) 2-word PMwCAS:
set status back => Frozen Bit isn't set
set meta entry to visiable and correct offset
7.1) if fails and Frozen bit is not set, read status and retry
7.2) if Frozen bit is set, abort and retry the insert
*/
/* ִ��Ҷ�ڵ����������� */
template<typename Key, typename Val>
int bz_node<Key, Val>::insert(bz_tree<Key, Val> * tree, Key * key, Val * val, uint32_t key_size, uint32_t total_size, uint32_t alloc_epoch)
{
	uint64_t * meta_arr = rec_meta_arr();
	uint32_t sorted_cnt = get_sorted_count(length_);
	uint32_t node_sz = get_node_size(length_);

	/* UNIKEY �������򲿷��Ƿ����ظ���ֵ */
	int bin_pos = binary_search(meta_arr, sorted_cnt, key);
	if (bin_pos != sorted_cnt && !key_cmp(meta_arr[bin_pos], key))
		return EUNIKEY;

	bool done, retry = false;
	uint64_t tmp_offset = alloc_epoch, meta_new;
	uint32_t rec_cnt, blk_sz;
	do
	{
		uint64_t status_rd = pmwcas_read(&status_);
		if (is_frozen(status_rd))
			return EFROZEN;
		rec_cnt = get_record_count(status_rd);
		blk_sz = get_block_size(status_rd);

		/* ������ */
		if (blk_sz + total_size + sizeof(uint64_t) * (1 + rec_cnt) + sizeof(*this) > node_sz)
			return EALLOCSIZE;

		/* UNIKEY �������򲿷��Ƿ����ظ���ֵ */
		for (auto i = sorted_cnt; i < rec_cnt; ++i)
		{
			if (is_visiable(meta_arr[i])) {
				if (!key_cmp(meta_arr[i], key))
					return EUNIKEY;
			}
			else if (!retry && get_offset(meta_arr[i]) == tmp_offset)
				retry = true;
		}

		/* ����record count��block size */
		uint64_t status_new = status_rd;
		set_record_count(status_new, rec_cnt + 1);
		set_block_size(status_new, blk_sz + total_size);
		
		/* ����offsetΪalloc_epoch, unset visiable bit */
		uint64_t meta_rd = pmwcas_read(meta_arr + rec_cnt);
		meta_new = meta_rd;
		set_offset(meta_new, tmp_offset);
		unset_visiable(meta_new);

		/* 2-word PMwCAS */
		mdesc_t mdesc = pmwcas_alloc(&tree->pool_);
		if (mdesc.is_null())
			return EPMWCASALLOC;
		pmwcas_add(mdesc, &status_, status_rd, status_new);
		pmwcas_add(mdesc, &meta_arr[rec_cnt], meta_rd, meta_new);
		done = pmwcas_commit(mdesc);
		pmwcas_free(mdesc);
	} while (!done);

	uint32_t new_offset = node_sz - blk_sz - total_size - 1;
	set_key(new_offset, key);
	set_value(new_offset + key_size, val);
	persist((char *)this + new_offset, total_size);

	if (retry) {
		auto i = sorted_cnt;
		while (i < rec_cnt)
		{
			if (is_visiable(meta_arr[i])) {
				if (!key_cmp(meta_arr[i], key)) {
					set_offset(meta_arr[rec_cnt], 0);
					persist(&meta_arr[rec_cnt], sizeof(uint64_t));
					return EUNIKEY;
				}
			}
			else if (get_offset(meta_arr[i]) == tmp_offset) {
				// Ǳ�ڵ�UNIKEY����������ȴ������
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
				continue;
			}
			++i;
		}
	}

	/* set visiable; real offset; key_len and tot_len */
	uint64_t meta_new_plus = meta_new;
	set_key_length(meta_new_plus, key_size);
	set_total_length(meta_new_plus, total_size);
	set_visiable(meta_new_plus);
	set_offset(meta_new_plus, new_offset);

	done = false;
	while (!done)
	{
		uint64_t status_rd = pmwcas_read(&status_);
		if (is_frozen(status_rd))
			return EFROZEN;
		mdesc_t mdesc = pmwcas_alloc(&tree->pool_);
		if (mdesc.is_null())
			return EPMWCASALLOC;
		pmwcas_add(mdesc, &meta_arr[rec_cnt], meta_new, meta_new_plus);
		pmwcas_add(mdesc, &status_, status_rd, status_rd);
		if (pmwcas_commit(mdesc))
			done = true;
		pmwcas_free(mdesc);
	}
	
	return 0;
}
/*
delete:
1) 2-word PMwCAS on
	meta entry: visiable = 0, offset = 0
	node status: delete size += data length
	1.1) if fails due to Frozen bit set, abort and retraverse
	1.2) otherwise, read and retry
*/

/*
update(swap pointer):
1) 3-word PMwCAS
	pointer in storage block
	meta entry status => competing delete
	node status => Frozen bit
*/

/*
read
1) binary search on sorted keys
2) linear scan on unsorted keys
3) return the record found
*/

/*
range scan: [beg_key, end_key)
one leaf node at a time
1) enter the epoch
2) construct a response_array(visiable and not deleted)
3) exit epoch
4) one-record-at-a-time
5) enter new epoch
6) greater than search the largest key in response array
*/

/*
consolidate:

trigger by
	either free space too small or deleted space too large

1) single-word PMwCAS on status => frozen bit
2) scan all valid records(visiable and not deleted)
	calculate node size = record size + free space
	2.1) if node size too large, do a split
	2.2) otherwise, allocate a new node N', copy records sorted, init header
3) use path stack to find Node's parent P
	3.1) if P's frozen, retraverse to locate P
4) 2-word PMwCAS
	r in P that points to N => N'
	P's status => detect concurrent freeze
5) N is ready for gc
6) 
*/


/* �״�ʹ��BzTree */
template<typename Key, typename Val>
void bz_tree<Key, Val>::first_use()
{
	pmwcas_first_use(&pool_);
	root_.set_null();
	persist(&root_, sizeof(uint64_t));
}

/* ��ʼ��BzTree */
template<typename Key, typename Val>
int bz_tree<Key, Val>::init(PMEMobjpool * pop, PMEMoid base_oid)
{
	rel_ptr<bz_node<Key, Val>>::set_base(base_oid);
	rel_ptr<rel_ptr<bz_node<Key, Val>>>::set_base(base_oid);
	pop_ = pop;
	int err = 0;
	if (err = pmwcas_init(&pool_, base_oid))
		return err;
	pmwcas_recovery(&pool_);
	return 0;
}

/* �չ� */
template<typename Key, typename Val>
void bz_tree<Key, Val>::finish()
{
	pmwcas_finish(&pool_);
}

template<typename Key, typename Val>
int bz_tree<Key, Val>::alloc_node(rel_ptr<rel_ptr<bz_node<Key, Val>>> addr, rel_ptr<bz_node<Key, Val>> expect, size_t size) {
	mdesc_t mdesc = pmwcas_alloc(&pool_);
	if (mdesc.is_null())
		return EPMWCASALLOC;
	rel_ptr<rel_ptr<bz_node<Key, Val>>> ptr = pmwcas_reserve<bz_node<Key, Val>>(mdesc, addr, expect);
	TX_BEGIN(pop_) {
		pmemobj_tx_add_range_direct(ptr.abs(), sizeof(uint64_t));
		*ptr = pmemobj_tx_alloc(size, TOID_TYPE_NUM(struct bz_node_fake));
	} TX_END;
	memset((*ptr).abs(), 0, size);
	set_node_size((*ptr)->length_, size);
	persist((*ptr).abs(), size);
	bool ret = pmwcas_commit(mdesc);
	pmwcas_free(mdesc);
	return ret ? 0 : EPMWCASFAIL;
}

template<typename Key, typename Val>
uint64_t * bz_node<Key, Val>::rec_meta_arr() {
	return (uint64_t*)((char*)this + sizeof(*this));
}
/* K-V getter and setter */
template<typename Key, typename Val>
Key * bz_node<Key, Val>::get_key(uint64_t meta) {
	return (Key*)((char*)this + get_offset(meta));
}
template<typename Key, typename Val>
void bz_node<Key, Val>::set_key(uint32_t offset, const Key *key) {
	Key * addr = (Key *)((char *)this + offset);
	if (typeid(Key) == typeid(char))
		strcpy((char *)addr, (char*)key);
	else
		*addr = *key;
}
template<typename Key, typename Val>
Val * bz_node<Key, Val>::get_value(uint64_t meta) {
	return (Val*)((char*)this + get_offset(meta) + get_key_length(meta));
}
template<typename Key, typename Val>
void bz_node<Key, Val>::set_value(uint32_t offset, const Val * val) {
	Val * addr = (Val *)((char *)this + offset);
	if (typeid(Val) == typeid(char))
		strcpy((char*)addr, (char*)val);
	else
		*addr = *val;
}

/* ��ֵ�ȽϺ��� */
template<typename Key, typename Val>
int bz_node<Key, Val>::key_cmp(uint64_t meta_entry, const Key * key) {
	const Key * k1 = get_key(meta_entry);
	if (typeid(Key) == typeid(char))
		return strcmp((char*)k1, (char*)key);
	if (*k1 == *key)
		return 0;
	return *k1 < *key ? -1 : 1;
}