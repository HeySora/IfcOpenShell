#include "IfcHdf5File.h"

#include "H5pubconf.h"

#ifndef H5_HAVE_FILTER_DEFLATE
#pragma message("warning: HDF5 compression support is recommended")
#endif

class UnmetDependencyException : public std::exception {};

void visit(void* buffer, H5::DataType* dt) {
	if (dt->getClass() == H5T_COMPOUND) {
		H5::CompType* ct = (H5::CompType*) dt;
		for (int i = 0; i < ct->getNmembers(); ++i) {
			std::cerr << ct->getMemberName(i) << " ";
			size_t offs = ct->getMemberOffset(i);
			H5::DataType dt2 = ct->getMemberDataType(i);
			visit((uint8_t*)buffer+offs, &dt2);
			dt2.close();
		}
	} else if (dt->getClass() == H5T_VLEN) {
		hvl_t* ht = (hvl_t*) buffer;
		H5::VarLenType* vt = (H5::VarLenType*) dt;
		H5::DataType dt2 = vt->getSuper();
		for (int i = 0; i < ht->len; ++i) {
			std::cerr << i << " ";
			visit((uint8_t*)ht->p + i * vt->getSize(), &dt2);
		}
		dt2.close();
	} else if (dt->getClass() == H5T_STRING) {
		char* c = *(char**)buffer;
		std::cerr << "'" << c << "'" << " ";
	}
}

H5::DataType* IfcParse::IfcHdf5File::map_type(const IfcParse::parameter_type* pt) {
	H5::DataType* h5_dt;
	if (pt->as_aggregation_type()) {
		h5_dt = new H5::VarLenType(map_type(pt->as_aggregation_type()->type_of_element()));
	} else if (pt->as_named_type()) {
		if (pt->as_named_type()->declared_type()->as_entity()) {
			h5_dt = new H5::DataType();
			h5_dt->copy(*instance_reference);
		} else {
			IfcSchema::Type::Enum ty = pt->as_named_type()->declared_type()->type();
			if (declared_types.find(ty) == declared_types.end()) {
				throw UnmetDependencyException();
			} else {
				h5_dt = new H5::DataType();
				h5_dt->copy(*declared_types[ty]);
			}
		}
	} else if (pt->as_simple_type()) {
		const H5::DataType* orig = default_types[pt->as_simple_type()->declared_type()];
		h5_dt = new H5::DataType();
		h5_dt->copy(*orig);
	} else {
		throw UnmetDependencyException();
	}
	return h5_dt;
}

H5::CompType* create_compound(const std::vector< IfcParse::IfcHdf5File::compound_member >& members) {
	size_t s = 0, o = 0;
	for (auto it = members.begin(); it != members.end(); ++it) {
		s += it->second->getSize();
	}

	H5::CompType* h5_dt = new H5::CompType(s);
	for (auto it = members.begin(); it != members.end(); ++it) {
		h5_dt->insertMember(it->first, o, *it->second);
		o += it->second->getSize();
	}

	return h5_dt;
}

H5::EnumType* create_enumeration(const std::vector<std::string>& items, int offset = 0) {
	size_t numbytes = 0;
	size_t size = items.size();
	while (size != 0) {
		size >>= 8;
		numbytes ++;
	}

	int i = offset;
	H5::EnumType* h5_enum = new H5::EnumType(numbytes);
	for (auto it = items.begin(); it != items.end(); ++it, ++i) {
		h5_enum->insert(it->c_str(), &i);
	}

	return h5_enum;
}

H5::EnumType* map_enumeration(const IfcParse::enumeration_type* en) {
	return create_enumeration(en->enumeration_items());
}

H5::DataType* IfcParse::IfcHdf5File::commit(H5::DataType* dt, const std::string& name) {
	dt->commit(schema_group, name);
	return dt;
}

H5::CompType* IfcParse::IfcHdf5File::map_entity(const IfcParse::entity* e) {
	std::vector<const IfcParse::entity::attribute*> attributes = e->all_attributes();
	std::vector<const IfcParse::entity::inverse_attribute*> inverse_attributes;
	if (settings_.instantiate_inverse()) {
		inverse_attributes = e->all_inverse_attributes();
	}
	
	std::vector< IfcParse::IfcHdf5File::compound_member > h5_attributes;
	h5_attributes.reserve(attributes.size() + inverse_attributes.size() + 2);

	h5_attributes.push_back(std::make_pair(std::string("set_unset_bitmap"), &H5::PredType::NATIVE_INT16));
    h5_attributes.push_back(std::make_pair(std::string("Entity-Instance-Identifier"), &H5::PredType::NATIVE_INT32));
	
	const std::vector<bool>& attributes_derived_in_subtype = e->derived();
	auto jt = attributes_derived_in_subtype.begin();

	for (auto it = attributes.begin(); it != attributes.end(); ++it, ++jt) {
		if (*jt) {
			continue;
		}
		const std::string& name = (*it)->name();
		const bool is_optional = (*it)->optional();
		const H5::DataType* type;
		if (overridden_types.find(name) != overridden_types.end()) {
			type = overridden_types.find(name)->second;
		} else {
			type = map_type((*it)->type_of_attribute());
		}
		h5_attributes.push_back(std::make_pair(name, type));
	}

	if (settings_.instantiate_inverse()) {
		for (auto it = inverse_attributes.begin(); it != inverse_attributes.end(); ++it) {
			const std::string& name = (*it)->name();
			H5::DataType* ir_copy = new H5::DataType();
			ir_copy->copy(*instance_reference);
			const H5::DataType* type = new H5::VarLenType(ir_copy);
			h5_attributes.push_back(std::make_pair(name, type));
		}
	}

	return create_compound(h5_attributes);
}

void IfcParse::IfcHdf5File::init_default_types() {
	{
		std::vector< compound_member > members;
		members.push_back( std::make_pair(std::string("_HDF5_dataset_index_"),  new H5::PredType(H5::PredType::NATIVE_INT16)) );
		members.push_back( std::make_pair(std::string("_HDF5_instance_index_"), new H5::PredType(H5::PredType::NATIVE_INT32)) );
		instance_reference = static_cast<H5::CompType*>(commit(create_compound(members), "_HDF_INSTANCE_REFERENCE_HANDLE_"));
	}

	object_reference_type = &H5::PredType::STD_REF_OBJ;

	{
		std::vector<std::string> names;
		names.push_back("BOOLEAN-FALSE");
		names.push_back("BOOLEAN-TRUE");
		default_types[simple_type::boolean_type] = create_enumeration(names);
	}

	{
		std::vector<std::string> names;
		names.push_back("LOGICAL-UNKNOWN");
		names.push_back("LOGICAL-FALSE");
		names.push_back("LOGICAL-TRUE");
		default_types[simple_type::logical_type] = create_enumeration(names, -1);
	}

	default_types[simple_type::binary_type]  = &H5::PredType::NATIVE_OPAQUE; // vlen?
	default_types[simple_type::real_type]    = &H5::PredType::NATIVE_DOUBLE;
	default_types[simple_type::number_type]  = &H5::PredType::NATIVE_DOUBLE;
	default_types[simple_type::string_type]  = new H5::StrType(H5::PredType::C_S1, H5T_VARIABLE);
	default_types[simple_type::integer_type] = &H5::PredType::NATIVE_INT;
	
	default_type_names[simple_type::logical_type] = "logical";
	default_type_names[simple_type::boolean_type] = "boolean";
	default_type_names[simple_type::binary_type]  = "binary";
	default_type_names[simple_type::real_type]    = "real";
	default_type_names[simple_type::number_type]  = "number";
	default_type_names[simple_type::string_type]  = "string";
	default_type_names[simple_type::integer_type] = "integer";

	default_cpp_type_names[IfcUtil::Argument_BOOL]   = "boolean";
	default_cpp_type_names[IfcUtil::Argument_DOUBLE] = "real";
	default_cpp_type_names[IfcUtil::Argument_STRING] = "string";
	default_cpp_type_names[IfcUtil::Argument_INT]    = "integer";

	if (settings_.fix_global_id()) {
		overridden_types["GlobalId"] = new H5::StrType(H5::PredType::C_S1, 22);
	}
	if (settings_.fix_cartesian_point()) {
		hsize_t dims = 3;
		overridden_types["Coordinates"] = new H5::ArrayType(*default_types[simple_type::real_type], 1, &dims);
		overridden_types["DirectionRatios"] = new H5::ArrayType(*default_types[simple_type::real_type], 1, &dims);
	}
}

void IfcParse::IfcHdf5File::visit_select(const IfcParse::select_type* pt, std::set<const IfcParse::declaration*>& leafs) {
	for (auto it = pt->select_list().begin(); it != pt->select_list().end(); ++it) {
		if ((*it)->as_select_type()) {
			visit_select((*it)->as_select_type(), leafs);
		} else {
			leafs.insert(*it);
		}
	}
}

std::string IfcParse::IfcHdf5File::flatten_aggregate_name(const IfcParse::parameter_type* at) const {
	if (at->as_aggregation_type()) {
		return std::string("aggregate-of-") + flatten_aggregate_name(at->as_aggregation_type()->type_of_element());
	} else if (at->as_simple_type()) {
		return default_type_names.find(at->as_simple_type()->declared_type())->second;
	} else if (at->as_named_type()) {
		// TODO: Unwind type name
		return at->as_named_type()->declared_type()->name();
	} else {
		throw;
	}
}

H5::DataType* IfcParse::IfcHdf5File::map_select(const IfcParse::select_type* pt) {
	if (settings_.instantiate_select()) return 0;

	std::set<const IfcParse::declaration*> leafs;
	visit_select(pt, leafs);

	bool all_entity_instance_refs = true;
	for (auto it = leafs.begin(); it != leafs.end(); ++it) {
		if (!(*it)->as_entity()) {
			all_entity_instance_refs = false;
			break;
		}
	}

	if (all_entity_instance_refs) {
		return 0;
	} else {
		std::set<std::string> member_names;
		std::vector<compound_member> h5_attributes;
		
		h5_attributes.push_back(std::make_pair(std::string("select_bitmap"), &H5::PredType::NATIVE_INT8));
		h5_attributes.push_back(std::make_pair(std::string("type_path"), default_types[simple_type::string_type]));

		for (auto it = leafs.begin(); it != leafs.end(); ++it) {
			std::string name;
			const H5::DataType* dt = 0;

			const IfcParse::declaration* decl = (*it);

			if ((*it)->as_type_declaration()) {
				while (decl->as_type_declaration()) {
					const IfcParse::parameter_type* pt = decl->as_type_declaration()->declared_type();
					const IfcParse::named_type* nt = pt->as_named_type();
					const IfcParse::simple_type* st = pt->as_simple_type();
					const IfcParse::aggregation_type* at = pt->as_aggregation_type();
					if (nt) {
						decl = nt->declared_type();
					} else if (st) {
						name = default_type_names[st->declared_type()];
						dt = default_types[st->declared_type()];
						break;
					} else if (at) {
						name = flatten_aggregate_name(at);
						dt = map_type(at);
						break;
					}
				}
			}

			if (!dt) {
				if ((*it)->as_entity()) {
					name = "instance";
					dt = instance_reference;
				} else if ((*it)->as_enumeration_type()) {
					name = (*it)->name();
					dt = declared_types[(*it)->type()];
				} else {
					throw;
				}
			}

			if (member_names.find(name) != member_names.end()) {
				continue;
			}
			member_names.insert(name);

			name += "-value";
			h5_attributes.push_back(std::make_pair(name, dt));
		}

		return create_compound(h5_attributes);
	}
}

void IfcParse::IfcHdf5File::advance(void*& ptr, size_t n) const {
	ptr = ( uint8_t*) ptr + n;
}

template <typename T>
void IfcParse::IfcHdf5File::write(void*& ptr, const T& t) const {
	*((T*)ptr) = t;
	advance(ptr, sizeof(T));
}

template <>
void IfcParse::IfcHdf5File::write(void*& ptr, const std::string& s) const {
	char* c = new(allocator.allocate(s.size()+1)) char [s.size()+1];
	strcpy(c, s.c_str());
	write(ptr, c);
}

template <unsigned int T> struct uint_of_size     {};
template <>               struct uint_of_size <1> { typedef  uint8_t type; };
template <>               struct uint_of_size <2> { typedef uint16_t type; };
template <>               struct uint_of_size <4> { typedef uint32_t type; };
template <>               struct uint_of_size <8> { typedef uint64_t type; };

template <unsigned int T> struct int_of_size     {};
template <>               struct int_of_size <1> { typedef  int8_t type; };
template <>               struct int_of_size <2> { typedef int16_t type; };
template <>               struct int_of_size <4> { typedef int32_t type; };
template <>               struct int_of_size <8> { typedef int64_t type; };

template <unsigned int T> struct float_of_size     {};
template <>               struct float_of_size <4> { typedef  float type; };
template <>               struct float_of_size <8> { typedef double type; };

template <typename T>
void IfcParse::IfcHdf5File::write_number_of_size(void*& ptr, size_t n, T i) const {
	if (!std::numeric_limits<T>::is_integer) {
		if (n == 4) {
			write(ptr, static_cast< float_of_size<4>::type > (i));
		} else if (n == 8) {
			write(ptr, static_cast< float_of_size<8>::type > (i));
		}
	} else if (std::numeric_limits<T>::is_signed) {
		if (n == 1) {
			write(ptr, static_cast< int_of_size<1>::type > (i));
		} else if (n == 2) {
			write(ptr, static_cast< int_of_size<2>::type > (i));
		} else if (n == 4) {
			write(ptr, static_cast< int_of_size<4>::type > (i));
		} else if (n == 8) {
			write(ptr, static_cast< int_of_size<8>::type > (i));
		}
	} else {
		if (n == 1) {
			write(ptr, static_cast< uint_of_size<1>::type > (i));
		} else if (n == 2) {
			write(ptr, static_cast< uint_of_size<2>::type > (i));
		} else if (n == 4) {
			write(ptr, static_cast< uint_of_size<4>::type > (i));
		} else if (n == 8) {
			write(ptr, static_cast< uint_of_size<8>::type > (i));
		}
	}
}

void IfcParse::IfcHdf5File::write_vlen_t(void*& ptr, size_t n_elements, void* vlen_data) const {
	advance(ptr, HOFFSET(hvl_t, len));
	void* temp_ptr;
	write_number_of_size(temp_ptr = ptr, sizeof(size_t), n_elements);
	advance(ptr, HOFFSET(hvl_t, p));
	write(ptr, vlen_data);
}

template <typename T>
void IfcParse::IfcHdf5File::write_aggregate(void*& ptr, const T& ts) const {
	size_t elem_size = get_datatype<typename T::value_type>()->getSize();
	size_t n_elements = ts.size();
	size_t size_in_bytes = elem_size * n_elements;
	void* aggr_data = allocator.allocate(size_in_bytes);
	void* aggr_ptr = aggr_data;
	for (T::const_iterator it = ts.begin(); it != ts.end(); ++it) {
		write_number_of_size(aggr_ptr, elem_size, *it);
	}
	write_vlen_t(ptr, n_elements, aggr_data);
}

template <>
void IfcParse::IfcHdf5File::write_aggregate(void*& ptr, const std::vector<std::string>& ts) const {
	size_t elem_size = sizeof(char*);
	size_t n_elements = ts.size();
	size_t size_in_bytes = elem_size * n_elements;
	void* aggr_data = allocator.allocate(size_in_bytes);
	void* aggr_ptr = aggr_data;
	for (std::vector<std::string>::const_iterator it = ts.begin(); it != ts.end(); ++it) {
		write(aggr_ptr, *it);
	}
	write_vlen_t(ptr, n_elements, aggr_data);
}

template <>
void IfcParse::IfcHdf5File::write_aggregate(void*& ptr, const IfcEntityList::ptr& ts) const {
	size_t elem_size = instance_reference->getSize();
	size_t n_elements = ts->size();
	size_t size_in_bytes = elem_size * n_elements;
	void* aggr_data = allocator.allocate(size_in_bytes);
	void* aggr_ptr = aggr_data;
	for (IfcEntityList::it it = ts->begin(); it != ts->end(); ++it) {
		auto ref = make_instance_reference(*it);
		// write_number_of_size(aggr_ptr, instance_reference->getMemberDataType(0).getSize(), ref.first);
		// write_number_of_size(aggr_ptr, instance_reference->getMemberDataType(1).getSize(), ref.second);
		// Hard-coded for efficiency
		write_number_of_size(aggr_ptr, 2, ref.first);
		write_number_of_size(aggr_ptr, 4, ref.second);
	}
	write_vlen_t(ptr, n_elements, aggr_data);
}

void IfcParse::IfcHdf5File::write_schema(const IfcParse::schema_definition& schema) {

	// From h5ex_g_compact.c
	// Compact groups?
	H5::FileAccPropList* plist = new H5::FileAccPropList();
	plist->setLibverBounds(H5F_LIBVER_LATEST, H5F_LIBVER_LATEST);

	// H5::FileCreatPropList* plist2 = new H5::FileCreatPropList();
	// H5Pset_file_space(plist2->getId(), H5F_FILE_SPACE_VFD, (hsize_t)0);
	
	file = new H5::H5File(name_, H5F_ACC_TRUNC, H5::FileCreatPropList::DEFAULT, *plist);

	schema_group = file->createGroup(schema.name() + "_encoding");
	population_group = file->createGroup("population");
	
	init_default_types();
			
	for (auto it = schema.enumeration_types().begin(); it != schema.enumeration_types().end(); ++it) {
		declared_types[(*it)->type()] = commit(map_enumeration((*it)), (*it)->name());
	}
		
	const std::vector<const type_declaration*>& ts = schema.type_declarations();
	std::set<const type_declaration*> processed;
	while (processed.size() < ts.size()) {
		for (auto it = ts.begin(); it != ts.end(); ++it) {
			const std::string& name = (*it)->name();
			if (processed.find(*it) != processed.end()) continue;
			try {
				declared_types[(*it)->type()] = commit(map_type((*it)->declared_type()), (*it)->name());
				processed.insert(*it);
			} catch(const UnmetDependencyException&) {}
			auto pt = (*it)->declared_type();
		}
	}

	for (auto it = schema.select_types().begin(); it != schema.select_types().end(); ++it) {
		H5::DataType* dt = map_select(*it);
		if (dt) {
			declared_types[(*it)->type()] = commit(dt, (*it)->name());
		} else {
			declared_types[(*it)->type()] = instance_reference;
		}
	}
		
	for (auto it = schema.entities().begin(); it != schema.entities().end(); ++it) {
		declared_types[(*it)->type()] = commit(map_entity(*it), (*it)->name());
	}

	H5::StrType schema_name_t;
	schema_name_t.copy(H5::PredType::C_S1);
	schema_name_t.setSize(schema.name().size());

	hsize_t schema_name_length = 1;
	H5::DataSpace schema_name_s(1, &schema_name_length);
	
	H5::Attribute attr = schema_group.createAttribute("iso_10303_26_data", schema_name_t, schema_name_s);
	attr.write(schema_name_t, schema.name());
	attr.close();
};

std::pair<size_t, size_t> IfcParse::IfcHdf5File::make_instance_reference(const IfcUtil::IfcBaseClass* instance) const {
#ifdef SORT_ON_NAME
	const std::vector<uint32_t>& es = sorted_entities.find(instance->declaration().type())->second;
#else
	const std::vector<IfcUtil::IfcBaseClass*>& es = sorted_entities.find(instance->declaration().type())->second;
#endif
	
	auto dataset_id = std::lower_bound(dataset_names.begin(), dataset_names.end(), instance->declaration().type());
#ifdef SORT_ON_NAME
	auto instance_id = std::lower_bound(es.begin(), es.end(), instance->data().id());
#else
	auto instance_id = std::lower_bound(es.begin(), es.end(), instance);
#endif
	
	size_t dataset_offset = std::distance(dataset_names.begin(), dataset_id);
	size_t instance_offset = std::distance(es.begin(), instance_id);

	return std::make_pair(dataset_offset, instance_offset);
}

void IfcParse::IfcHdf5File::write_select(void*& ptr, IfcUtil::IfcBaseClass* instance, const H5::CompType* datatype) const {
	int member_index = -1;

	if (instance->declaration().as_entity()) {
		member_index = datatype->getMemberIndex("instance-value");
		size_t offset = datatype->getMemberOffset(member_index);
		auto ref = make_instance_reference(instance);
		void* ptr_member = (uint8_t*) ptr + offset;
		// write_number_of_size(ptr_member, instance_reference->getMemberDataType(0).getSize(), ref.first);
		// write_number_of_size(ptr_member, instance_reference->getMemberDataType(1).getSize(), ref.second);
		write_number_of_size(ptr_member, 2, ref.first);
		write_number_of_size(ptr_member, 4, ref.second);
	} else {
		Argument& wrapped_data = *instance->data().getArgument(0);
		IfcUtil::ArgumentType ty = wrapped_data.type();
		if (default_cpp_type_names.find(ty) == default_cpp_type_names.end()) {
			Logger::Message(Logger::LOG_ERROR, "Unsupported select valuation encountered", instance);
		} else {
			std::string member_name = default_cpp_type_names.find(ty)->second + "-value";
			member_index = datatype->getMemberIndex(member_name);
			size_t offset = datatype->getMemberOffset(member_index);
			H5::DataType memberdt = datatype->getMemberDataType(member_index);
			size_t member_size = memberdt.getSize();
			memberdt.close();
			void* ptr_member = (uint8_t*) ptr + offset;
			switch(wrapped_data.type()) {
				case IfcUtil::Argument_BOOL: {
					bool instance = wrapped_data;
					write_number_of_size(ptr_member, member_size, static_cast<uint8_t>(instance ? 1 : 0));
					break; }
				case IfcUtil::Argument_DOUBLE: {
					double d = wrapped_data;
					write_number_of_size(ptr_member, member_size, d);
					break; }
				case IfcUtil::Argument_STRING: {
					std::string s = wrapped_data;
					write(ptr_member, s);
					break; }
				case IfcUtil::Argument_INT: {
					int i = wrapped_data;
					write_number_of_size(ptr_member, member_size, i);
					break; }
				default:
					Logger::Message(Logger::LOG_ERROR, "Unsupported select valuation encountered", instance);
					break;
			}
		}							
	}

	if (member_index == -1) {
		member_index = 0;
	} else {
		member_index -= 2; // select_bitmap, type_path
	}

	void* temp_ptr = ptr;
	write_number_of_size(temp_ptr, H5::PredType::NATIVE_INT8.getSize(), member_index);
	write(temp_ptr, instance->declaration().name());

	// TODO: Should string be set to "", or keep as null?
	// std::cout << datatype->getMemberIndex("string-value") << std::endl;

	advance(ptr, datatype->getSize());
}

void IfcParse::IfcHdf5File::write_population(IfcFile& f) {
	std::set<IfcSchema::Type::Enum> tys;

	// Wth was this?
	// this->file->close();
	
	std::set<IfcSchema::Type::Enum> types_with_instiated_selected;

	for (auto it = f.begin(); it != f.end(); ++it) {
		IfcSchema::Type::Enum ty = it->second->declaration().type();
		tys.insert(ty);
		// This already is sorted on entity instance name
#ifdef SORT_ON_NAME
		sorted_entities[ty].push_back(static_cast<uint32_t>(it->first));
#else
		sorted_entities[ty].push_back(it->second);
#endif

#ifndef SORT_ON_NAME
		if (settings_.instantiate_select()) {
			bool has=false;
			for (unsigned i = 0; i < it->second->data().getArgumentCount(); ++i) {
				Argument* attr = it->second->data().getArgument(i);
				if (attr->type() == IfcUtil::Argument_ENTITY_INSTANCE) {
					IfcUtil::IfcBaseClass* inst = *attr;
					if (!inst->declaration().as_entity()) {
						IfcSchema::Type::Enum ty2 = inst->declaration().type();
						tys.insert(ty2);
						sorted_entities[ty2].push_back(inst);
						has = true;
					}
				} else if (attr->type() == IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE) {
					IfcEntityList::ptr insts = *attr;
					for (auto it = insts->begin(); it != insts->end(); ++it) {
						if (!(*it)->declaration().as_entity()) {
							IfcSchema::Type::Enum ty2 = (*it)->declaration().type();
							tys.insert(ty2);
							sorted_entities[ty2].push_back(*it);
							has = true;
						}
					}
				}
			}
			if (has) {
				types_with_instiated_selected.insert(ty);
			} else {
				static_cast<IfcParse::Entity*>(&it->second->data())->Unload();
			}
		}
#endif
	}

	dataset_names.assign(tys.begin(), tys.end());
	std::sort(dataset_names.begin(), dataset_names.end());

	{
		hsize_t dataset_names_length = dataset_names.size();
		H5::DataSpace dataset_names_s(1, &dataset_names_length);

		H5::Attribute attr = schema_group.createAttribute("iso_10303_26_data_set_names", *default_types[simple_type::string_type], dataset_names_s);
		char** attr_data = (char**) allocator.allocate(sizeof(char*) * dataset_names_length);
		size_t i = 0;
		for (auto it = dataset_names.begin(); it != dataset_names.end(); ++it, ++i) {
			std::string nm = IfcSchema::Type::ToString(*it);
			attr_data[i] = (char*) allocator.allocate(nm.size() + 1);
			strcpy(attr_data[i], nm.c_str());
		}
		attr.write(*default_types[simple_type::string_type], attr_data);
		attr.close();
	}

#ifdef SORT_ON_NAME
	for (auto it = sorted_entities.begin(); it != sorted_entities.end(); ++it) {
		std::sort(it->second.begin(), it->second.end());
	}
#endif
	
	for (auto it = dataset_names.begin(); it != dataset_names.end(); ++it) {

		// std::cout << "begin inner loop" << std::endl;
		// std::cin.get();

		// if (*it != IfcSchema::Type::IfcUnitAssignment) continue;

		std::cerr << IfcSchema::Type::ToString(*it) << std::endl;

#ifdef SORT_ON_NAME
		std::vector<IfcUtil::IfcBaseClass*> es;
		es.reserve(sorted_entities.find(*it)->second.size());
		for (auto jt = sorted_entities.find(*it)->second.begin(); jt != sorted_entities.find(*it)->second.end(); ++jt) {
			es.push_back(f.entityById(*jt));
		}
#else
		const std::vector<IfcUtil::IfcBaseClass*>& es = sorted_entities.find(*it)->second;
#endif
		size_t datatype_size = declared_types[*it]->getSize();
		hsize_t dims = es.size();
		
		hsize_t chunk;
		if (settings_.chunk_size() > 0 && settings_.chunk_size() < dims) {
			chunk = static_cast<hsize_t>(settings_.chunk_size());
		} else {
			chunk = dims;
		}

		const H5::DSetCreatPropList* plist;
		// H5O_MESG_MAX_SIZE = 65536
		const bool compact = es.size() * datatype_size < (1 << 15);
		if (settings_.compress() && !compact) { 
			H5::DSetCreatPropList* plist_ = new H5::DSetCreatPropList;
			plist_->setChunk(1, &chunk);
			// D'oh. Order is significant, according to h5ex_d_shuffle.c
			plist_->setShuffle();
			plist_->setDeflate(9);
			plist = plist_;
		} else if (compact) {
			H5::DSetCreatPropList* plist_ = new H5::DSetCreatPropList;
			// Set compact according to h5ex_d_compact.c
			plist_->setLayout(H5D_COMPACT);
			plist = plist_;
		} else if (chunk != dims){
			H5::DSetCreatPropList* plist_ = new H5::DSetCreatPropList;
			plist_->setChunk(1, &chunk);
			plist = plist_;
		} else {
			plist = &H5::DSetCreatPropList::DEFAULT;
		}

		H5::DataSpace space(1, &dims);
		H5::DataSet ds = population_group.createDataSet(IfcSchema::Type::ToString(*it) + "_instances", *declared_types[*it], space, *plist);
		
		size_t dataset_size = declared_types[*it]->getSize() * static_cast<size_t>(dims);
		void* data = allocator.allocate(dataset_size);

		std::cerr << dataset_size << std::endl;
		
		void* ptr = data;
		const H5::CompType* dt = (H5::CompType*) declared_types[*it];
		int ind = 0;

		for (auto jt = es.begin(); jt != es.end(); ++jt, ++ind) {
			void* start = ptr;

			int member_idx = 0;

			H5::DataType member_type;

			IfcAbstractEntity& dat = (*jt)->data();
			const declaration& decl = (*jt)->declaration();			

			if (!decl.as_entity()) {
				if (!settings_.instantiate_select()) throw;
				// This is a type
				auto q = dt->getClass();
				auto s = dt->getSize();
				const Argument& attr_value = *dat.getArgument(0);
				if (*dt == *default_types[simple_type::real_type]) {
					double d = attr_value;
					write_number_of_size(ptr, s, d);
				} else if (*dt == *default_types[simple_type::string_type]) {
					std::string s = attr_value;
					write(ptr, s);
				} else if (*dt == *default_types[simple_type::integer_type]) {
					int i = attr_value;
					write_number_of_size(ptr, s, i);
				} else if (*dt == *default_types[simple_type::boolean_type] ||
					        *dt == *default_types[simple_type::logical_type]) 
				{
					bool b = attr_value;
					write_number_of_size(ptr, s, static_cast<uint8_t>(b ? 1 : 0));
				} else {
					throw;
				}
				continue;
			}

			std::vector<const IfcParse::entity::attribute*> attributes = decl.as_entity()->all_attributes();
			std::vector<const IfcParse::entity::inverse_attribute*> inverse_attributes;
			if (settings_.instantiate_inverse()) {
				inverse_attributes = decl.as_entity()->all_inverse_attributes();
			}
			const std::vector<bool>& attributes_derived_in_subtype = decl.as_entity()->derived();
			std::vector<bool>::const_iterator is_derived = attributes_derived_in_subtype.begin();

			member_type = dt->getMemberDataType(member_idx++);
			uint32_t set_unset_mask = 0;
			void* set_unset_ptr = ptr;
			size_t set_unset_size = member_type.getSize();
			// skip for now write later
			advance(ptr, set_unset_size);
			member_type.close();
			
			member_type = dt->getMemberDataType(member_idx++);
			const unsigned int inst_name = static_cast<unsigned int>(dat.id());
			write_number_of_size(ptr, member_type.getSize(), inst_name);
			member_type.close();

			//                        ----v-----   In some IFC files there are extra superfluous attributes in the instantiation. For example FJK haus.
			const size_t attr_count = (std::min)(attributes.size(), (size_t) dat.getArgumentCount());
			for (unsigned i = 0; i < attr_count; ++i, ++is_derived) {
				if (*is_derived) {
					continue;
				}

				const IfcParse::entity::attribute* schema_attr = attributes[i];
				const std::string& attribute_name = schema_attr->name();
				Argument& attr_value = *dat.getArgument(i);

				member_type = dt->getMemberDataType(member_idx++);

				if (attr_value.isNull()) {
					if (member_type == *default_types[simple_type::string_type]) {
						// HDF5 otherwise crashes on derefencing a zero pointer for the string type
						write(ptr, new(allocator.allocate(1)) char(0));
					} else {
						memset(ptr, 0, member_type.getSize());
						advance(ptr, member_type.getSize());
					}
				} else {
					set_unset_mask |= 1 << i;
					if (settings_.fix_global_id() && attribute_name == "GlobalId") {
						std::string s = attr_value;
						memcpy(static_cast<char*>(ptr), s.c_str(), s.size());
						advance(ptr, s.length());
					} else if (settings_.fix_cartesian_point() && (attribute_name == "Coordinates" || attribute_name == "DirectionRatios")) {
						std::vector<double> ds = attr_value;
						for (auto it = ds.begin(); it != ds.end(); ++it) {
							write_number_of_size(ptr, default_types[simple_type::real_type]->getSize(), *it);
						}
						if (ds.size() == 2) {
							write_number_of_size(ptr, default_types[simple_type::real_type]->getSize(), std::numeric_limits<double>::quiet_NaN());
						}
					} else if (member_type == *instance_reference) {
						IfcUtil::IfcBaseClass* v = attr_value;
						auto ref = make_instance_reference(v);
						// write_number_of_size(ptr, instance_reference->getMemberDataType(0).getSize(), ref.first);
						// write_number_of_size(ptr, instance_reference->getMemberDataType(1).getSize(), ref.second);
						write_number_of_size(ptr, 2, ref.first);
						write_number_of_size(ptr, 4, ref.second);
					} else if (member_type == *default_types[simple_type::real_type]) {
						double d = attr_value;
						write_number_of_size(ptr, member_type.getSize(), d);
					} else if (member_type == *default_types[simple_type::string_type]) {
						std::string s = attr_value;
						write(ptr, s);
					} else if (member_type == *default_types[simple_type::integer_type]) {
						int i = attr_value;
						write_number_of_size(ptr, member_type.getSize(), i);
					} else if (member_type == *default_types[simple_type::boolean_type] ||
					           member_type == *default_types[simple_type::logical_type]) 
					{
						bool b = attr_value;
						write_number_of_size(ptr, member_type.getSize(), static_cast<uint8_t>(b ? 1 : 0));
					} else if (member_type.getClass() == H5T_ENUM) {
						// NB: Note that boolean and logical are also enums
						// NB2: In IfcOpenShell an enum value can be read as a string
						std::string s = attr_value;
						const std::vector<std::string>& enum_values = schema_attr->type_of_attribute()->as_named_type()->declared_type()->as_enumeration_type()->enumeration_items();
						size_t d = std::distance(enum_values.begin(), std::find(enum_values.begin(), enum_values.end(), s));
						write_number_of_size(ptr, member_type.getSize(), d);
					} else if (member_type.getClass() == H5T_VLEN) {
						const parameter_type* pt = schema_attr->type_of_attribute();
						while (pt->as_named_type()) {
							pt = pt->as_named_type()->declared_type()->as_type_declaration()->declared_type();
						}
						const named_type* nt = pt->as_aggregation_type()->type_of_element()->as_named_type();
						if (nt && nt->declared_type()->as_select_type()) {
							const H5::DataType* datatype = declared_types[nt->declared_type()->type()];
							IfcEntityList::ptr es = attr_value;
							if (datatype == instance_reference) {
								// For a SELECTs with only ENTITY leaves, a blind instance reference type is used
								write_aggregate(ptr, es);
							} else {
								size_t size_in_bytes = datatype->getSize();
								size_t num_elements = es->size();
								void* buffer_ptr;
								// void* buffer = buffer_ptr = new uint8_t[size_in_bytes * num_elements];
								void* buffer = buffer_ptr = allocator.allocate(size_in_bytes * num_elements);
								memset(buffer, 0, size_in_bytes * num_elements);
								for (auto it = es->begin(); it != es->end(); ++it) {
									write_select(buffer_ptr, *it, static_cast<const H5::CompType*>(datatype));
								}
								write_vlen_t(ptr, num_elements, buffer);
							}

						} else {
							if (sizeof(hvl_t) != member_type.getSize()) throw;
							switch(attr_value.type()) {
							case IfcUtil::Argument_AGGREGATE_OF_INT: {
								std::vector<int> ds = attr_value;
								write_aggregate(ptr, ds);
								break; }
							case IfcUtil::Argument_AGGREGATE_OF_DOUBLE: {
								std::vector<double> ds = attr_value;
								write_aggregate(ptr, ds);
								break; }
							case IfcUtil::Argument_AGGREGATE_OF_ENTITY_INSTANCE: {
								IfcEntityList::ptr es = attr_value;
								write_aggregate(ptr, es);
								break; }
							case IfcUtil::Argument_AGGREGATE_OF_STRING: {
								std::vector<std::string> ss = attr_value;
								write_aggregate(ptr, ss);
								break; }
							default:
								// Can be an empty list in which case parser does not know type
								if (attr_value.size() > 0) {
									Logger::Message(Logger::LOG_ERROR, "Unsupported aggregate encountered", *jt);
								}
								memset(ptr, 0, member_type.getSize());
								ptr = (uint8_t*) ptr + member_type.getSize();
							}
						}
					} else if (member_type.getClass() == H5T_COMPOUND) {
						memset(ptr, 0, member_type.getSize());
						
						IfcUtil::ArgumentType ty = attr_value.type();
						if (ty != IfcUtil::Argument_ENTITY_INSTANCE) throw;
						IfcUtil::IfcBaseClass* b = attr_value;

						write_select(ptr, attr_value, static_cast<H5::CompType*>(&member_type));
					}
				}

				member_type.close();
			}

			for (auto it = inverse_attributes.begin(); it != inverse_attributes.end(); ++it) {
				member_type = dt->getMemberDataType(member_idx++);

				auto q = member_type.getClass();
				if (member_type.getClass() != H5T_VLEN) {
					std::cerr << dt->getMemberName(member_idx - 1) << " ";
					std::cerr << "Inverse attribute must be vlen" << std::endl;
				}
				const IfcParse::entity* entity_ref = (*it)->entity_reference();
				const IfcParse::entity::attribute* attribute_ref = (*it)->attribute_reference();
				IfcEntityList::ptr instances = f.getInverse(dat.id(), entity_ref->type(), entity_ref->attribute_index(attribute_ref));
				if (instances->size() == 0) {
					memset(ptr, 0, member_type.getSize());
					advance(ptr, member_type.getSize());
				} else {
					write_aggregate(ptr, instances);
				}

				member_type.close();
			}

			write_number_of_size(set_unset_ptr, set_unset_size, set_unset_mask);			
			const size_t written_length = (uint8_t*) ptr - (uint8_t*) start;
			
			if (written_length != datatype_size) {
				std::cerr << "Written " << written_length << " bytes, but expected " << datatype_size << std::endl;
			}
		}

		/* for (size_t i = 0; i < dataset_size; ++i) {
			std::cout << std::hex << (int)((uint8_t*)data)[i] << " ";
		} */

		// visit(data, declared_types[*it]);
		ds.write(data, *declared_types[*it]);
		H5Dvlen_reclaim(declared_types[*it]->getId(), space.getId(), H5P_DEFAULT, data);

		ds.close();
		space.close();

		if (plist != &H5::DSetCreatPropList::DEFAULT) {
			// ->close() doesn't work due to const, hack hack hack
			H5Pclose(plist->getId());
		}

		// allocator.free();
		delete[] data;
		
		for (auto it = es.begin(); it != es.end(); ++it) {
			if (types_with_instiated_selected.find((**it).declaration().type()) == types_with_instiated_selected.end()) {
				// Instances possibly refering to embedded simple type instantiations are not freed
				static_cast<IfcParse::Entity*>(&(**it).data())->Unload();
			}
		}

	}
}