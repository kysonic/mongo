// Tests that a change stream will correctly unwind applyOps entries generated by a transaction.
// @tags: [uses_transactions]

(function() {
    "use strict";

    load("jstests/libs/change_stream_util.js");
    load("jstests/libs/collection_drop_recreate.js");  // For assert[Drop|Create]Collection.

    function testChangeStreamsWithTransactions(watchWholeDb) {
        let cst = new ChangeStreamTest(db);

        const otherCollName = "change_stream_apply_ops_2";
        const coll = assertDropAndRecreateCollection(db, "change_stream_apply_ops");
        assertDropAndRecreateCollection(db, otherCollName);

        const otherDbName = "change_stream_apply_ops_db";
        const otherDbCollName = "someColl";
        assertDropAndRecreateCollection(db.getSiblingDB(otherDbName), otherDbCollName);

        // Insert a document that gets deleted as part of the transaction.
        const kDeletedDocumentId = 0;
        coll.insert({_id: kDeletedDocumentId, a: "I was here before the transaction"});

        let collArg = coll;
        if (watchWholeDb) {
            collArg = 1;
        }
        let changeStream =
            cst.startWatchingChanges({pipeline: [{$changeStream: {}}], collection: collArg});

        const sessionOptions = {causalConsistency: false};
        const session = db.getMongo().startSession(sessionOptions);
        const sessionDb = session.getDatabase(db.getName());
        const sessionColl = sessionDb[coll.getName()];

        session.startTransaction({readConcern: {level: "snapshot"}, writeConcern: {w: "majority"}});
        assert.commandWorked(sessionColl.insert({_id: 1, a: 0}));
        assert.commandWorked(sessionColl.insert({_id: 2, a: 0}));

        // One insert on a collection that we're not watching. This should be skipped in the change
        // stream.
        assert.commandWorked(
            sessionDb[otherCollName].insert({_id: 0, a: "Doc on other collection"}));

        // One insert on a database we're not watching. Should also be skipped.
        assert.commandWorked(session.getDatabase(otherDbName)[otherDbCollName].insert(
            {_id: 0, a: "SHOULD NOT READ THIS"}));

        assert.commandWorked(sessionColl.updateOne({_id: 1}, {$inc: {a: 1}}));

        assert.commandWorked(sessionColl.deleteOne({_id: kDeletedDocumentId}));

        session.commitTransaction();

        // Do applyOps on the collection that we care about. This is an "external" applyOps, though
        // (not run as part of a transaction) so its entries should be skipped in the change
        // stream. This checks that applyOps that don't have an 'lsid' and 'txnNumber' field do not
        // get unwound.
        assert.commandWorked(db.runCommand({
            applyOps: [
                {op: "i", ns: coll.getFullName(), o: {_id: 3, a: "SHOULD NOT READ THIS"}},
            ]
        }));

        // Drop the collection. This will trigger an "invalidate" event.
        assert.commandWorked(db.runCommand({drop: coll.getName()}));

        // Check for the first insert.
        let change = cst.getOneChange(changeStream);
        assert.eq(change.fullDocument._id, 1);
        assert.eq(change.operationType, "insert", tojson(change));
        const firstChangeTxnNumber = change.txnNumber;
        const firstChangeLsid = change.lsid;
        assert.eq(typeof firstChangeLsid, "object");

        // Check for the second insert.
        change = cst.getOneChange(changeStream);
        assert.eq(change.fullDocument._id, 2);
        assert.eq(change.operationType, "insert", tojson(change));
        assert.eq(firstChangeTxnNumber.valueOf(), change.txnNumber);
        assert.eq(0, bsonWoCompare(firstChangeLsid, change.lsid));

        if (watchWholeDb) {
            // We should see the insert on the other collection.
            change = cst.getOneChange(changeStream);
            assert.eq(change.fullDocument._id, 0);
            assert.eq(change.operationType, "insert", tojson(change));
            assert.eq(firstChangeTxnNumber.valueOf(), change.txnNumber);
            assert.eq(0, bsonWoCompare(firstChangeLsid, change.lsid));
            assert.eq(change.ns.coll, otherCollName);
            assert.eq(change.ns.db, db.getName());
        }

        // Check for the update.
        change = cst.getOneChange(changeStream);
        assert.eq(tojson(change.updateDescription.updatedFields), tojson({"a": 1}));
        assert.eq(change.operationType, "update", tojson(change));
        assert.eq(firstChangeTxnNumber.valueOf(), change.txnNumber);
        assert.eq(0, bsonWoCompare(firstChangeLsid, change.lsid));

        // Check for the delete.
        change = cst.getOneChange(changeStream);
        assert.eq(change.documentKey._id, kDeletedDocumentId);
        assert.eq(change.operationType, "delete", tojson(change));
        assert.eq(firstChangeTxnNumber.valueOf(), change.txnNumber);
        assert.eq(0, bsonWoCompare(firstChangeLsid, change.lsid));

        // The drop should have invalidated the change stream.
        cst.assertNextChangesEqual({
            cursor: changeStream,
            expectedChanges: [{operationType: "invalidate"}],
            expectInvalidate: true
        });

        cst.cleanUp();
    }

    // TODO: SERVER-34302 should allow us to simplify this test, so we're not required to
    // explicitly run both against a single collection and against the whole DB.
    testChangeStreamsWithTransactions(false);
    testChangeStreamsWithTransactions(true);
}());