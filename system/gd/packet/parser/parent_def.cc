/*
 * Copyright 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "parent_def.h"

#include "fields/all_fields.h"
#include "util.h"

ParentDef::ParentDef(std::string name, FieldList fields) : TypeDef(name), fields_(fields), parent_(nullptr){};
ParentDef::ParentDef(std::string name, FieldList fields, ParentDef* parent)
    : TypeDef(name), fields_(fields), parent_(parent){};

void ParentDef::AddParentConstraint(std::string field_name, std::variant<int64_t, std::string> value) {
  // NOTE: This could end up being very slow if there are a lot of constraints.
  const auto& parent_params = parent_->GetParamList();
  const auto& constrained_field = parent_params.GetField(field_name);
  if (constrained_field == nullptr) {
    ERROR() << "Attempting to constrain field " << field_name << " in parent " << parent_->name_
            << ", but no such field exists.";
  }

  if (constrained_field->GetFieldType() == ScalarField::kFieldType) {
    if (!std::holds_alternative<int64_t>(value)) {
      ERROR(constrained_field) << "Attemting to constrain a scalar field to an enum value in " << parent_->name_;
    }
  } else if (constrained_field->GetFieldType() == EnumField::kFieldType) {
    if (!std::holds_alternative<std::string>(value)) {
      ERROR(constrained_field) << "Attemting to constrain an enum field to a scalar value in " << parent_->name_;
    }
    const auto& enum_def = static_cast<EnumField*>(constrained_field)->GetEnumDef();
    if (!enum_def.HasEntry(std::get<std::string>(value))) {
      ERROR(constrained_field) << "No matching enumeration \"" << std::get<std::string>(value)
                               << "for constraint on enum in parent " << parent_->name_ << ".";
    }

    // For enums, we have to qualify the value using the enum type name.
    value = enum_def.GetTypeName() + "::" + std::get<std::string>(value);
  } else {
    ERROR(constrained_field) << "Field in parent " << parent_->name_ << " is not viable for constraining.";
  }

  parent_constraints_.insert(std::pair(field_name, value));
}

// Assign all size fields to their corresponding variable length fields.
// Will crash if
//  - there aren't any fields that don't match up to a field.
//  - the size field points to a fixed size field.
//  - if the size field comes after the variable length field.
void ParentDef::AssignSizeFields() {
  for (const auto& field : fields_) {
    DEBUG() << "field name: " << field->GetName();

    if (field->GetFieldType() != SizeField::kFieldType && field->GetFieldType() != CountField::kFieldType) {
      continue;
    }

    const SizeField* size_field = static_cast<SizeField*>(field);
    // Check to see if a corresponding field can be found.
    const auto& var_len_field = fields_.GetField(size_field->GetSizedFieldName());
    if (var_len_field == nullptr) {
      ERROR(field) << "Could not find corresponding field for size/count field.";
    }

    // Do the ordering check to ensure the size field comes before the
    // variable length field.
    for (auto it = fields_.begin(); *it != size_field; it++) {
      DEBUG() << "field name: " << (*it)->GetName();
      if (*it == var_len_field) {
        ERROR(var_len_field, size_field) << "Size/count field must come before the variable length field it describes.";
      }
    }

    if (var_len_field->GetFieldType() == PayloadField::kFieldType) {
      const auto& payload_field = static_cast<PayloadField*>(var_len_field);
      payload_field->SetSizeField(size_field);
      continue;
    }

    if (var_len_field->GetFieldType() == ArrayField::kFieldType) {
      const auto& array_field = static_cast<ArrayField*>(var_len_field);
      array_field->SetSizeField(size_field);
      continue;
    }

    // If we've reached this point then the field wasn't a variable length field.
    // Check to see if the field is a variable length field
    std::cerr << "Can not use size/count in reference to a fixed size field.\n";
    abort();
  }
}

void ParentDef::SetEndianness(bool is_little_endian) {
  is_little_endian_ = is_little_endian;
}

// Get the size. You scan specify without_payload in order to exclude payload fields as children will be overriding it.
Size ParentDef::GetSize(bool without_payload) const {
  auto size = Size();

  for (const auto& field : fields_) {
    if (without_payload &&
        (field->GetFieldType() == PayloadField::kFieldType || field->GetFieldType() == BodyField::kFieldType)) {
      continue;
    }

    // The offset to the field must be passed in as an argument for dynamically sized custom fields.
    if (field->GetFieldType() == CustomField::kFieldType && field->GetSize().has_dynamic()) {
      std::stringstream custom_field_size;

      // Custom fields are special as their size field takes an argument.
      custom_field_size << field->GetSize().dynamic_string() << "(begin()";

      // Check if we can determine offset from begin(), otherwise error because by this point,
      // the size of the custom field is unknown and can't be subtracted from end() to get the
      // offset.
      auto offset = GetOffsetForField(field->GetName(), false);
      if (offset.empty()) {
        ERROR(field) << "Custom Field offset can not be determined from begin().";
      }

      if (offset.bits() % 8 != 0) {
        ERROR(field) << "Custom fields must be byte aligned.";
      }
      if (offset.has_bits()) custom_field_size << " + " << offset.bits() / 8;
      if (offset.has_dynamic()) custom_field_size << " + " << offset.dynamic_string();
      custom_field_size << ")";

      size += custom_field_size.str();
      continue;
    }

    size += field->GetSize();
  }

  if (parent_ != nullptr) {
    size += parent_->GetSize(true);
  }

  return size;
}

// Get the offset until the field is reached, if there is no field
// returns an empty Size. from_end requests the offset to the field
// starting from the end() iterator. If there is a field with an unknown
// size along the traversal, then an empty size is returned.
Size ParentDef::GetOffsetForField(std::string field_name, bool from_end) const {
  // Check first if the field exists.
  if (fields_.GetField(field_name) == nullptr) {
    if (field_name != "payload" && field_name != "body") {
      ERROR() << "Can't find a field offset for nonexistent field named: " << field_name;
    } else {
      return Size();
    }
  }

  // We have to use a generic lambda to conditionally change iteration direction
  // due to iterator and reverse_iterator being different types.
  auto size_lambda = [&](auto from, auto to) -> Size {
    auto size = Size(0);
    for (auto it = from; it != to; it++) {
      // We've reached the field, end the loop.
      if ((*it)->GetName() == field_name) break;
      const auto& field = *it;
      // If there was a field that wasn't the payload with an unknown size,
      // return an empty Size.
      if (field->GetSize().empty()) {
        return Size();
      }
      size += field->GetSize();
    }
    return size;
  };

  // Change iteration direction based on from_end.
  auto size = Size();
  if (from_end)
    size = size_lambda(fields_.rbegin(), fields_.rend());
  else
    size = size_lambda(fields_.begin(), fields_.end());
  if (size.empty()) return Size();

  // We need the offset until a payload or body field.
  if (parent_ != nullptr) {
    auto parent_payload_offset = parent_->GetOffsetForField("payload", from_end);
    if (!parent_payload_offset.empty()) {
      size += parent_payload_offset;
    } else {
      parent_payload_offset = parent_->GetOffsetForField("body", from_end);
      if (!parent_payload_offset.empty()) {
        size += parent_payload_offset;
      } else {
        return Size();
      }
    }
  }

  return size;
}

FieldList ParentDef::GetParamList() const {
  FieldList params;

  std::set<std::string> param_types = {
      ScalarField::kFieldType, EnumField::kFieldType,    ArrayField::kFieldType,
      CustomField::kFieldType, PayloadField::kFieldType,
  };

  if (parent_ != nullptr) {
    auto parent_params = parent_->GetParamList().GetFieldsWithTypes(param_types);

    // Do not include constrained fields in the params
    for (const auto& field : parent_params) {
      if (parent_constraints_.find(field->GetName()) == parent_constraints_.end()) {
        params.AppendField(field);
      }
    }
  }
  // Add our parameters.
  return params.Merge(fields_.GetFieldsWithTypes(param_types));
}

void ParentDef::GenMembers(std::ostream& s) const {
  // Add the parameter list.
  for (int i = 0; i < fields_.size(); i++) {
    if (fields_[i]->GenBuilderParameter(s)) {
      s << "_;";
    }
  }
}

void ParentDef::GenSize(std::ostream& s) const {
  auto header_fields = fields_.GetFieldsBeforePayloadOrBody();
  auto footer_fields = fields_.GetFieldsAfterPayloadOrBody();

  s << "protected:";
  s << "size_t BitsOfHeader() const {";
  s << "return 0";

  if (parent_ != nullptr) {
    if (parent_->GetDefinitionType() == Type::PACKET) {
      s << " + " << parent_->name_ << "Builder::BitsOfHeader() ";
    } else {
      s << " + " << parent_->name_ << "::BitsOfHeader() ";
    }
  }

  for (const auto& field : header_fields) {
    Size field_size = field->GetBuilderSize();
    if (field_size.has_bits()) {
      s << " + " << field_size.bits();
    }
    if (field_size.has_dynamic()) {
      s << " + " << field_size.dynamic_string();
    }
  }
  s << ";";

  s << "}\n\n";

  s << "size_t BitsOfFooter() const {";
  s << "return 0";
  for (const auto& field : footer_fields) {
    Size field_size = field->GetBuilderSize();
    if (field_size.has_bits()) {
      s << " + " << field_size.bits();
    }
    if (field_size.has_dynamic()) {
      s << " + " << field_size.dynamic_string();
    }
  }

  if (parent_ != nullptr) {
    if (parent_->GetDefinitionType() == Type::PACKET) {
      s << " + " << parent_->name_ << "Builder::BitsOfFooter() ";
    } else {
      s << " + " << parent_->name_ << "::BitsOfFooter() ";
    }
  }
  s << ";";
  s << "}\n\n";

  if (fields_.HasPayload()) {
    s << "size_t GetPayloadSize() const {";
    s << "if (payload_ != nullptr) {return payload_->size();}";
    s << "else { return size() - (BitsOfHeader() + BitsOfFooter()) / 8;}";
    s << ";}\n\n";
  }

  s << "public:";
  s << "virtual size_t size() const override {";
  s << "return (BitsOfHeader() / 8)";
  if (fields_.HasPayload()) {
    s << "+ payload_->size()";
  }
  s << " + (BitsOfFooter() / 8);";
  s << "}\n";
}

void ParentDef::GenSerialize(std::ostream& s) const {
  auto header_fields = fields_.GetFieldsBeforePayloadOrBody();
  auto footer_fields = fields_.GetFieldsAfterPayloadOrBody();

  s << "protected:";
  s << "void SerializeHeader(BitInserter&";
  if (parent_ != nullptr || header_fields.size() != 0) {
    s << " i ";
  }
  s << ") const {";

  if (parent_ != nullptr) {
    if (parent_->GetDefinitionType() == Type::PACKET) {
      s << parent_->name_ << "Builder::SerializeHeader(i);";
    } else {
      s << parent_->name_ << "::SerializeHeader(i);";
    }
  }

  for (const auto& field : header_fields) {
    if (field->GetFieldType() == SizeField::kFieldType) {
      const auto& field_name = ((SizeField*)field)->GetSizedFieldName();
      const auto& sized_field = fields_.GetField(field_name);
      if (sized_field == nullptr) {
        ERROR(field) << __func__ << ": Can't find sized field named " << field_name;
      }
      if (sized_field->GetFieldType() == PayloadField::kFieldType) {
        s << "size_t payload_bytes = GetPayloadSize();";
        std::string modifier = ((PayloadField*)sized_field)->size_modifier_;
        if (modifier != "") {
          s << "static_assert((" << modifier << ")%8 == 0, \"Modifiers must be byte-aligned\");";
          s << "payload_bytes = payload_bytes + (" << modifier << ") / 8;";
        }
        s << "ASSERT(payload_bytes < (static_cast<size_t>(1) << " << field->GetSize().bits() << "));";
        s << "insert(static_cast<" << field->GetDataType() << ">(payload_bytes), i," << field->GetSize().bits() << ");";
      } else {
        if (sized_field->GetFieldType() != ArrayField::kFieldType) {
          ERROR(field) << __func__ << ": Unhandled sized field type for " << field_name;
        }
        const auto& array_name = field_name + "_";
        const ArrayField* array = (ArrayField*)sized_field;
        s << "size_t " << array_name + "bytes =  0;";
        if (array->element_size_ == -1) {
          s << "for (auto elem : " << array_name << ") {";
          s << array_name + "bytes += elem.size(); }";
        } else {
          s << array_name + "bytes = ";
          s << array_name << ".size() * (" << array->element_size_ << " / 8);";
        }
        s << "ASSERT(" << array_name + "bytes < (1 << " << field->GetSize().bits() << "));";
        s << "insert(" << array_name << "bytes";
        s << array->GetSizeModifier() << ", i, ";
        s << field->GetSize().bits() << ");";
      }
    } else if (field->GetFieldType() == ChecksumStartField::kFieldType) {
      const auto& field_name = ((ChecksumStartField*)field)->GetStartedFieldName();
      const auto& started_field = fields_.GetField(field_name);
      if (started_field == nullptr) {
        ERROR(field) << __func__ << ": Can't find checksum field named " << field_name << "(" << field->GetName()
                     << ")";
      }
      s << "auto shared_checksum_ptr = std::make_shared<" << started_field->GetDataType() << ">();";
      s << started_field->GetDataType() << "::Initialize(*shared_checksum_ptr);";
      s << "i.RegisterObserver(packet::ByteObserver(";
      s << "[shared_checksum_ptr](uint8_t byte){" << started_field->GetDataType()
        << "::AddByte(*shared_checksum_ptr, byte);},";
      s << "[shared_checksum_ptr](){ return static_cast<uint64_t>(" << started_field->GetDataType()
        << "::GetChecksum(*shared_checksum_ptr));}));";
    } else if (field->GetFieldType() == CountField::kFieldType) {
      const auto& array_name = ((SizeField*)field)->GetSizedFieldName() + "_";
      s << "insert(" << array_name << ".size(), i, " << field->GetSize().bits() << ");";
    } else {
      field->GenInserter(s);
    }
  }
  s << "}\n\n";

  s << "void SerializeFooter(BitInserter&";
  if (parent_ != nullptr || footer_fields.size() != 0) {
    s << " i ";
  }
  s << ") const {";

  for (const auto& field : footer_fields) {
    field->GenInserter(s);
  }
  if (parent_ != nullptr) {
    if (parent_->GetDefinitionType() == Type::PACKET) {
      s << parent_->name_ << "Builder::SerializeFooter(i);";
    } else {
      s << parent_->name_ << "::SerializeFooter(i);";
    }
  }
  s << "}\n\n";

  s << "public:";
  s << "virtual void Serialize(BitInserter& i) const override {";
  s << "SerializeHeader(i);";
  if (fields_.HasPayload()) {
    s << "payload_->Serialize(i);";
  }
  s << "SerializeFooter(i);";

  s << "}\n";
}
